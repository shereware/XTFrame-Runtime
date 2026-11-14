#include "xt/gpu/world_layers.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "damage_internal.h"
#include "xt/asset_resolver.h"
#include "xt/gpu/frame_builder.h"

/// gpu::WorldLayerBuilder and buildWorldLayerQuads (xt/gpu/world_layers.h; Docs/design/
/// 33-world-layers.md §33.3-§33.4, Docs/tasks/T-701.md).

namespace xt::gpu {

namespace {

constexpr uint64_t kFnvBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv1a(uint64_t hash, const void* data, size_t count) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

/// The largest side of a layer target: the frame's scissors are 16-bit signed surface pixels.
constexpr uint32_t kMaxLayerSide = 16384;

/// T-703 (33 §33.13 item 4): whether drawing `frame` into a layer target needs the target's own
/// stencil (transients bring theirs: TransientTargetDesc::needStencil). Not needed when every
/// command of the surface passes leaves the stencil alone and passes its test on a stencil that
/// stays 0 (cleared with the target): StencilOp::None, or TestEqualRef at clip depth 0.
bool frameUsesStencil(const GpuFrame& frame) {
    for (const Pass& pass : frame.passes) {
        if (pass.target != kSurfaceTarget) {
            continue;
        }
        const size_t end = std::min<size_t>(size_t(pass.firstCmd) + pass.cmdCount, frame.cmds.size());
        for (size_t i = pass.firstCmd; i < end; ++i) {
            const DrawCmd& c = frame.cmds[i];
            if (c.shading == Shading::StencilOnly) {
                return true;
            }
            if (c.stencilOp == StencilOp::None ||
                (c.stencilOp == StencilOp::TestEqualRef && c.stencilRef == 0)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

}  // namespace

struct WorldLayerBuilder::Impl {
    /// One layer's retained state. Slots are never destroyed before shutdown: a layer that goes
    /// away leaves its slot (builder, frame capacity) for the next new layer.
    struct Slot {
        bool used = false;
        bool seen = false;      ///< named by the list of the build in progress
        bool drawn = false;     ///< `hash` is what the target holds (or will after submit())
        bool pending = false;   ///< `frame` waits for submit()
        bool external = false;  ///< draws a host texture: redrawn every build
        bool stencil = false;   ///< `texture` was acquired with a stencil attachment (T-703)
        uint16_t source = 0;
        uint32_t id = 0;
        uint64_t hash = 0;
        TextureHandle texture = 0;
        uint16_t width = 0, height = 0;
        bool inited = false;
        FrameBuilder builder;
        GpuFrame frame;
    };

    /// A range the build in progress made a frame for (so a glyph atlas reset can make it again).
    struct Built {
        Slot* slot;
        const WorldLayerRange* range;
        float density;
    };

    GpuDevice* device = nullptr;
    std::vector<Built> builtScratch;
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<WorldLayer> layers;
    std::vector<uint64_t> cmdHashes;
    /// The slot of each entry of `layers` (same index).
    std::vector<Slot*> layerSlots;
#ifndef XT_GPU_NO_TEXT
    xt::text::GlyphAtlas* glyphAtlas = nullptr;
    GlyphAtlasTextures atlasPages;
#endif
    bool edgeFringe = true;
    float density = 0.0f;
    bool unsupportedReported = false;
    BuildStatus status = BuildStatus::Ok;
    bool wantsAnother = false;
    uint64_t totalSubmitted = 0;
    Stats stats;
    std::string lastError;

    Slot* find(uint16_t source, uint32_t id) {
        for (const std::unique_ptr<Slot>& s : slots) {
            if (s->used && s->source == source && s->id == id) {
                return s.get();
            }
        }
        return nullptr;
    }

    Slot* take(uint16_t source, uint32_t id) {
        Slot* slot = nullptr;
        for (const std::unique_ptr<Slot>& s : slots) {
            if (!s->used) {
                slot = s.get();
                break;
            }
        }
        if (slot == nullptr) {
            slots.push_back(std::make_unique<Slot>());
            slot = slots.back().get();
        }
        slot->used = true;
        slot->source = source;
        slot->id = id;
        slot->drawn = false;
        slot->pending = false;
        slot->hash = 0;
        if (!slot->inited) {
            slot->builder.init(*device);
            slot->inited = true;
        }
        slot->builder.setEdgeFringe(edgeFringe);
#ifndef XT_GPU_NO_TEXT
        slot->builder.setGlyphAtlas(glyphAtlas, &atlasPages);
#endif
        return slot;
    }

    void releaseTarget(Slot& slot) {
        if (slot.texture != 0 && device != nullptr) {
            device->releaseLayerTarget(slot.texture);
        }
        slot.texture = 0;
        slot.stencil = false;
        slot.width = slot.height = 0;
        slot.drawn = false;
        slot.pending = false;
    }

    void drop(Slot& slot) {
        releaseTarget(slot);
        slot.builder.trim();  // its image textures and caches; the builder itself is kept
        slot.used = false;
    }

    /// True when the commands of `range` draw a host (`engine://`) texture, whose pixels change
    /// without the commands changing.
    static bool drawsExternal(const CommandList& list, const WorldLayerRange& range, const IrView* ir) {
        const IrView* src = sourceIr(list, range.source, ir);
        if (src == nullptr) {
            return false;
        }
        const size_t end =
            std::min<size_t>(size_t(range.firstCommand) + range.commandCount, list.commands.size());
        for (size_t i = range.firstCommand; i < end; ++i) {
            const Command& cmd = list.commands[i];
            AssetId asset = kInvalidAsset;
            switch (cmd.kind) {
                case CmdKind::DrawImage:
                    asset = list.images[cmd.index].asset;
                    break;
                case CmdKind::DrawImageQuad:
                    asset = list.imageQuads[cmd.index].asset;
                    break;
                case CmdKind::DrawParticles:
                    asset = list.particles[cmd.index].texture;
                    break;
                case CmdKind::DrawImageMesh:
                    asset = list.imageMeshes[cmd.index].asset;
                    break;
                default:
                    break;
            }
            if (asset != kInvalidAsset && isExternalAsset(asset, *src)) {
                return true;
            }
        }
        return false;
    }

    /// True when the range holds a custom effect (its output follows `CommandList::time`).
    static bool hasCustomEffect(const CommandList& list, const WorldLayerRange& range) {
        const size_t end =
            std::min<size_t>(size_t(range.firstCommand) + range.commandCount, list.commands.size());
        for (size_t i = range.firstCommand; i < end; ++i) {
            const Command& cmd = list.commands[i];
            if (cmd.kind != CmdKind::PushEffect) {
                continue;
            }
            const EffectCmd& fx = list.effectCmds[cmd.index];
            for (uint32_t e = 0; e < fx.effectCount && size_t(fx.effectOffset) + e < list.effects.size();
                 ++e) {
                if (list.effects[fx.effectOffset + e].kind == EffectKind::Custom) {
                    return true;
                }
            }
        }
        return false;
    }

    void buildSlot(Slot& slot, const CommandList& list, const WorldLayerRange& range, float layerDensity,
                   TextShaper* shaper, AssetResolver* assets, const IrView* ir, PerfTracker* perf) {
        slot.builder.buildRange(list, range.firstCommand, range.commandCount, layerDensity, shaper, assets,
                                ir, slot.frame, perf);
        slot.pending = true;
        slot.drawn = true;
        const char* err = slot.builder.lastError();
        if (err != nullptr && err[0] != '\0' && lastError.empty()) {
            lastError = err;
        }
    }

    bool build(const CommandList& list, TextShaper* shaper, AssetResolver* assets, const IrView* ir,
               PerfTracker* perf) {
        lastError.clear();
        layers.clear();
        layerSlots.clear();
        stats = Stats{};
        wantsAnother = false;
        status = BuildStatus::Ok;
        if (device == nullptr) {
            lastError = "WorldLayerBuilder::init was not called";
            status = BuildStatus::NotInitialized;
            return false;
        }
        if (list.worldLayers.empty()) {
            lastError = "the command list has no world layers (Context::setWorldLayerOutput is off)";
            status = BuildStatus::NoLayers;
            return false;
        }
        hashCommandList(list, cmdHashes);
        const float baseDensity = density > 0.0f ? density : list.devicePixelRatio;
        uint32_t maxSide = device->maxTextureSize();
        maxSide = maxSide == 0 ? kMaxLayerSide : std::min(maxSide, kMaxLayerSide);
        for (const std::unique_ptr<Slot>& s : slots) {
            s->seen = false;
        }
        std::vector<Built>& built = builtScratch;
        built.clear();

        for (const WorldLayerRange& range : list.worldLayers) {
            if (range.commandCount == 0 || !(range.rect.w > 0.0f) || !(range.rect.h > 0.0f) ||
                size_t(range.firstCommand) + range.commandCount > list.commands.size()) {
                continue;  // nothing to draw (an empty base layer)
            }
            // The density, lowered when the texture would not fit the device.
            float layerDensity = baseDensity;
            const float longest = std::max(range.rect.w, range.rect.h) * layerDensity;
            if (longest > float(maxSide)) {
                layerDensity *= float(maxSide) / longest;
            }
            const uint32_t w =
                std::clamp<uint32_t>(uint32_t(std::ceil(range.rect.w * layerDensity)), 1, maxSide);
            const uint32_t h =
                std::clamp<uint32_t>(uint32_t(std::ceil(range.rect.h * layerDensity)), 1, maxSide);

            Slot* slot = find(range.source, range.id);
            if (slot == nullptr) {
                slot = take(range.source, range.id);
            }
            slot->seen = true;
            if (slot->width != w || slot->height != h) {
                // Another size: the target goes; a new one is acquired once the frame is built
                // (T-703: only then is it known whether the frame uses the stencil).
                releaseTarget(*slot);
                slot->width = uint16_t(w);
                slot->height = uint16_t(h);
                slot->builder.resize(int(w), int(h));
            }

            // What the layer's pixels depend on: its commands (by content), where its rectangle
            // sits, the density and the fringe setting; a custom effect also on the clock.
            struct Header {
                float rect[4];
                float density;
                uint32_t width, height, fringe;
            };
            const Header header{{range.rect.x, range.rect.y, range.rect.w, range.rect.h},
                                layerDensity,
                                w,
                                h,
                                edgeFringe ? 1u : 0u};
            uint64_t hash = fnv1a(kFnvBasis, &header, sizeof(header));
            hash = fnv1a(hash, cmdHashes.data() + range.firstCommand,
                         size_t(range.commandCount) * sizeof(uint64_t));
            if (hasCustomEffect(list, range)) {
                hash = fnv1a(hash, &list.time, sizeof(list.time));
            }
            const bool external = drawsExternal(list, range, ir);
            const bool unchanged = slot->drawn && slot->texture != 0 && slot->hash == hash && !external &&
                                   !slot->builder.wantsAnotherFrame();
            slot->hash = hash;
            slot->external = external;
            uint8_t changed = 0;
            if (!unchanged) {
                buildSlot(*slot, list, range, layerDensity, shaper, assets, ir, perf);
                built.push_back(Built{slot, &range, layerDensity});
                changed = 1;
            }

            WorldLayer layer;
            layer.id = range.id;
            layer.order = range.order;
            layer.world3 = range.world3;
            layer.rect = range.rect;
            layer.texture = 0;  // set once the targets are acquired, below
            layer.texWidth = slot->width;
            layer.texHeight = slot->height;
            layer.backfaceHidden = range.backfaceHidden;
            layer.changed = changed;
            layer.source = range.source;
            layer.opacity = range.opacity;
            layer.density = layerDensity;
            layers.push_back(layer);
            layerSlots.push_back(slot);
        }

#ifndef XT_GPU_NO_TEXT
        // A glyph atlas reset (or growth) during one layer's build moved the glyphs the frames
        // built before it point at: those are built again (now every glyph is in the atlas; a
        // second reset means the frame's glyphs exceed it, which each builder handles itself).
        bool reset = false;
        for (const Built& b : built) {
            reset = b.slot->builder.takeGlyphAtlasReset() || reset;
        }
        if (reset) {
            for (const Built& b : built) {
                buildSlot(*b.slot, list, *b.range, b.density, shaper, assets, ir, perf);
                (void)b.slot->builder.takeGlyphAtlasReset();
            }
        }
#endif
        // T-703 (33 §33.13 item 4): each frame built gets a target with a stencil attachment only
        // when it uses the stencil; when that changes the target is acquired anew (the layer was
        // built, so it is drawn into the new one).
        for (const Built& b : built) {
            Slot& slot = *b.slot;
            const bool need = frameUsesStencil(slot.frame);
            if (slot.texture != 0 && slot.stencil == need) {
                continue;
            }
            if (slot.texture != 0) {
                device->releaseLayerTarget(slot.texture);
                slot.texture = 0;
            }
            slot.texture = device->acquireLayerTargetWithStencil(slot.id, slot.width, slot.height, need);
            if (slot.texture == 0) {
                lastError = unsupportedReported
                                ? "world layers: no layer target"
                                : "world layers are not supported by the device (no layer target)";
                unsupportedReported = true;
                status = BuildStatus::Unsupported;
                for (const std::unique_ptr<Slot>& s : slots) {
                    if (s->used && (!s->seen || s->texture == 0)) {
                        drop(*s);
                    }
                }
                layers.clear();
                layerSlots.clear();
                return false;
            }
            slot.stencil = need;
            ++stats.targetsCreated;
        }
        for (size_t i = 0; i < layers.size(); ++i) {
            layers[i].texture = layerSlots[i]->texture;
            layers[i].stencil = layerSlots[i]->stencil ? 1 : 0;
            stats.stencilLayers += layerSlots[i]->stencil ? 1u : 0u;
        }
        for (const Built& b : built) {
            stats.drawCommands += static_cast<uint32_t>(b.slot->frame.cmds.size());
            wantsAnother = wantsAnother || b.slot->builder.wantsAnotherFrame();
        }
        stats.built = static_cast<uint32_t>(built.size());
        stats.layers = static_cast<uint32_t>(layers.size());
        for (const std::unique_ptr<Slot>& s : slots) {
            if (s->used && !s->seen) {
                drop(*s);  // the layer is gone
            }
            if (s->used) {
                stats.targetBytes += uint64_t(s->width) * uint64_t(s->height) * 4u;
            }
        }
        return true;
    }

    void submit() {
        stats.submitted = 0;
        if (device == nullptr) {
            return;
        }
        for (const std::unique_ptr<Slot>& s : slots) {
            if (!s->used || !s->pending || s->texture == 0) {
                continue;
            }
            device->submitLayer(s->texture, s->frame);
            s->pending = false;
            ++stats.submitted;
            ++totalSubmitted;
        }
    }

    void shutdown() {
        for (const std::unique_ptr<Slot>& s : slots) {
            releaseTarget(*s);
            if (s->inited) {
                s->builder.shutdown();
                s->inited = false;
            }
            s->used = false;
        }
        slots.clear();
        layers.clear();
        layerSlots.clear();
#ifndef XT_GPU_NO_TEXT
        if (device != nullptr) {
            for (TextureHandle& t : atlasPages.textures) {
                device->destroyTexture(t);
                t = 0;
            }
        }
        atlasPages.size = 0;
#endif
        device = nullptr;
    }
};

WorldLayerBuilder::WorldLayerBuilder() : m_impl(std::make_unique<Impl>()) {}

WorldLayerBuilder::~WorldLayerBuilder() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

void WorldLayerBuilder::init(GpuDevice& device) {
    if (m_impl->device != nullptr && m_impl->device != &device) {
        m_impl->shutdown();
    }
    m_impl->device = &device;
}

void WorldLayerBuilder::shutdown() { m_impl->shutdown(); }

#ifndef XT_GPU_NO_TEXT
void WorldLayerBuilder::setGlyphAtlas(xt::text::GlyphAtlas* atlas) {
    if (atlas == m_impl->glyphAtlas) {
        return;
    }
    m_impl->glyphAtlas = atlas;
    // Another atlas: the shared pages hold the old one's glyphs.
    if (m_impl->device != nullptr) {
        for (TextureHandle& t : m_impl->atlasPages.textures) {
            m_impl->device->destroyTexture(t);
            t = 0;
        }
    }
    m_impl->atlasPages.size = 0;
    for (const std::unique_ptr<Impl::Slot>& s : m_impl->slots) {
        if (s->inited) {
            s->builder.setGlyphAtlas(atlas, &m_impl->atlasPages);
        }
        s->drawn = false;
    }
}
#endif

void WorldLayerBuilder::setEdgeFringe(bool enabled) {
    if (enabled == m_impl->edgeFringe) {
        return;
    }
    m_impl->edgeFringe = enabled;
    for (const std::unique_ptr<Impl::Slot>& s : m_impl->slots) {
        if (s->inited) {
            s->builder.setEdgeFringe(enabled);
        }
    }
}

void WorldLayerBuilder::setDensity(float density) {
    m_impl->density = (density > 0.0f && std::isfinite(density)) ? density : 0.0f;
}

float WorldLayerBuilder::density() const { return m_impl->density; }

bool WorldLayerBuilder::build(const CommandList& list, TextShaper* shaper, AssetResolver* assets,
                              const IrView* ir, PerfTracker* perf) {
    return m_impl->build(list, shaper, assets, ir, perf);
}

WorldLayerBuilder::BuildStatus WorldLayerBuilder::status() const { return m_impl->status; }

void WorldLayerBuilder::submit() { m_impl->submit(); }

std::span<const WorldLayer> WorldLayerBuilder::layers() const { return m_impl->layers; }

WorldLayerBuilder::Stats WorldLayerBuilder::stats() const { return m_impl->stats; }

uint64_t WorldLayerBuilder::totalSubmitted() const { return m_impl->totalSubmitted; }

bool WorldLayerBuilder::wantsAnotherFrame() const { return m_impl->wantsAnother; }

void WorldLayerBuilder::invalidateAll() {
    for (const std::unique_ptr<Impl::Slot>& s : m_impl->slots) {
        s->drawn = false;
        if (s->inited) {
            s->builder.invalidateAllAssets();
        }
    }
}

const char* WorldLayerBuilder::lastError() const { return m_impl->lastError.c_str(); }

// -------------------------------------------------------------------------------------------
// buildWorldLayerQuads.
// -------------------------------------------------------------------------------------------

void buildWorldLayerQuads(std::span<const WorldLayer> layers, const Mat4& boxToSurface, int surfaceWidth,
                          int surfaceHeight, bool clear, GpuFrame& out) {
    out.clear();
    out.targetWidth = surfaceWidth;
    out.targetHeight = surfaceHeight;
    Pass pass;
    pass.target = kSurfaceTarget;
    pass.clear = clear ? 1 : 0;
    pass.viewW = surfaceWidth;
    pass.viewH = surfaceHeight;
    if (surfaceWidth <= 0 || surfaceHeight <= 0) {
        out.passes.push_back(pass);
        return;
    }

    struct Quad {
        const WorldLayer* layer;
        float depth;
        float x[4], y[4], w[4];  ///< homogeneous surface pixels of the four corners
        /// The plane's map (u, v, 1) -> (X, Y, W): rows of three coefficients.
        float px[3], py[3], pw[3];
    };
    // The depth W of `plane` under the screen point (sx, sy); false when the plane is seen edge-on
    // there or the point lies behind the eye.
    const auto planeDepthAt = [](const Quad& plane, float sx, float sy, float& depth) {
        const float a = plane.px[0] - sx * plane.pw[0], b = plane.px[1] - sx * plane.pw[1];
        const float c = plane.py[0] - sy * plane.pw[0], d = plane.py[1] - sy * plane.pw[1];
        const float e = sx * plane.pw[2] - plane.px[2], f = sy * plane.pw[2] - plane.py[2];
        const float det = a * d - b * c;
        if (!(std::fabs(det) > 1e-12f)) {
            return false;
        }
        const float u = (e * d - b * f) / det, v = (a * f - e * c) / det;
        depth = plane.pw[0] * u + plane.pw[1] * v + plane.pw[2];
        return depth > 1e-6f && std::isfinite(depth);
    };
    // -1: every corner of `q` is in front of `plane` (nearer along its own view ray), +1: every
    // corner is behind it, 0: on the plane, across it, or not decidable.
    const auto sideOf = [&planeDepthAt](const Quad& q, const Quad& plane) {
        int behind = 0, front = 0;
        for (int i = 0; i < 4; ++i) {
            float depth = 0;
            if (!planeDepthAt(plane, q.x[i] / q.w[i], q.y[i] / q.w[i], depth)) {
                return 0;
            }
            const float tolerance = 1e-4f * std::max(depth, q.w[i]);
            behind += q.w[i] > depth + tolerance ? 1 : 0;
            front += q.w[i] < depth - tolerance ? 1 : 0;
        }
        return behind > 0 && front == 0 ? 1 : (front > 0 && behind == 0 ? -1 : 0);
    };
    // True when `a` has to be drawn before `b`: `a` lies behind `b`'s plane, or `b` in front of
    // `a`'s (Newell's two plane tests; the layers are few).
    const auto drawnBefore = [&sideOf](const Quad& a, const Quad& b) {
        return sideOf(a, b) > 0 || sideOf(b, a) < 0;
    };
    // At most kMaxWorldLayers per Document; a small fixed batch keeps this allocation-free.
    constexpr size_t kMaxQuads = 256;
    Quad quads[kMaxQuads];
    size_t count = 0;
    for (const WorldLayer& layer : layers) {
        if (count >= kMaxQuads || layer.texture == 0 || !(layer.opacity > 0.0f)) {
            continue;
        }
        Quad q{};
        q.layer = &layer;
        const Mat4 full = mat4Mul(boxToSurface, layer.world3);
        const float cx[4] = {layer.rect.x, layer.rect.x + layer.rect.w, layer.rect.x + layer.rect.w,
                             layer.rect.x};
        const float cy[4] = {layer.rect.y, layer.rect.y, layer.rect.y + layer.rect.h,
                             layer.rect.y + layer.rect.h};
        bool ok = true;
        float depth = 0;
        for (int i = 0; i < 4; ++i) {
            q.x[i] = full.m[0] * cx[i] + full.m[1] * cy[i] + full.m[3];
            q.y[i] = full.m[4] * cx[i] + full.m[5] * cy[i] + full.m[7];
            q.w[i] = full.m[12] * cx[i] + full.m[13] * cy[i] + full.m[15];
            if (!(q.w[i] > 1e-6f) || !std::isfinite(q.x[i]) || !std::isfinite(q.y[i])) {
                ok = false;
            }
            depth += 0.25f * q.w[i];
        }
        if (!ok) {
            continue;
        }
        q.px[0] = full.m[0];
        q.px[1] = full.m[1];
        q.px[2] = full.m[3];
        q.py[0] = full.m[4];
        q.py[1] = full.m[5];
        q.py[2] = full.m[7];
        q.pw[0] = full.m[12];
        q.pw[1] = full.m[13];
        q.pw[2] = full.m[15];
        if (layer.backfaceHidden != 0) {
            // Seen from behind when the projected quad's winding is reversed (y down: the front
            // runs clockwise on screen, a positive shoelace sum).
            float area = 0;
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) & 3;
                area += (q.x[i] / q.w[i]) * (q.y[j] / q.w[j]) - (q.x[j] / q.w[j]) * (q.y[i] / q.w[i]);
            }
            if (!(area > 0.0f)) {
                continue;
            }
        }
        q.depth = depth;
        quads[count++] = q;
    }
    // Paint order: first by the centre's depth (the farthest first, equal depths in paint order),
    // then corrected by the plane tests -- a small layer in front of a large one can have the
    // farther centre once the camera turns. At each position the first remaining quad that no
    // other remaining quad has to precede is taken (the depth order's first on a cycle).
    std::sort(quads, quads + count, [](const Quad& a, const Quad& b) {
        if (a.depth != b.depth) {
            return a.depth > b.depth;
        }
        if (a.layer->source != b.layer->source) {
            return a.layer->source < b.layer->source;
        }
        return a.layer->order < b.layer->order;
    });
    for (size_t i = 0; i + 1 < count; ++i) {
        size_t pick = i;
        for (size_t j = i; j < count; ++j) {
            bool free = true;
            for (size_t k = i; k < count && free; ++k) {
                free = k == j || !drawnBefore(quads[k], quads[j]);
            }
            if (free) {
                pick = j;
                break;
            }
        }
        if (pick != i) {
            const Quad chosen = quads[pick];
            for (size_t k = pick; k > i; --k) {
                quads[k] = quads[k - 1];
            }
            quads[i] = chosen;
        }
    }

    pass.firstCmd = 0;
    static const float kU[4] = {0, 1, 1, 0};
    static const float kV[4] = {0, 0, 1, 1};
    for (size_t i = 0; i < count; ++i) {
        const Quad& q = quads[i];
        const float o = std::clamp(q.layer->opacity, 0.0f, 1.0f);
        const uint32_t c = static_cast<uint32_t>(o * 255.0f + 0.5f);
        const uint32_t rgba = c | (c << 8) | (c << 16) | (c << 24);  // premultiplied white x opacity
        DrawCmd cmd;
        cmd.baseVertex = static_cast<uint32_t>(out.vertices.size());
        cmd.vertexCount = 4;
        cmd.firstIndex = static_cast<uint32_t>(out.indices.size());
        cmd.indexCount = 6;
        cmd.texture = q.layer->texture;
        cmd.shading = Shading::Texture;
        cmd.blend = Blend::SrcOver;
        cmd.sampler = Sampler::LinearClamp;
        cmd.clipX = 0;
        cmd.clipY = 0;
        cmd.clipW = static_cast<int16_t>(std::min(surfaceWidth, 32767));
        cmd.clipH = static_cast<int16_t>(std::min(surfaceHeight, 32767));
        for (int v = 0; v < 4; ++v) {
            out.vertices.push_back(Vertex{q.x[v], q.y[v], q.w[v], kU[v], kV[v], rgba});
        }
        for (uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) {
            out.indices.push_back(index);
        }
        out.cmds.push_back(cmd);
    }
    pass.cmdCount = static_cast<uint32_t>(out.cmds.size());
    out.passes.push_back(pass);
}

}  // namespace xt::gpu

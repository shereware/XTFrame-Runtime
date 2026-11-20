#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include "point_bounds.h"
#include "xt/asset_resolver.h"
#include "xt/context.h"
#include "xt/project3d.h"

/// Context::render()'s walk of the finished list for the external (`engine://`) textures it samples
/// (Docs/design/23-host-integration.md §23.ah, Docs/tasks/T-746.md): AssetResolver::externalTextureUse()
/// -- drawn or not, the largest device size -- and the damage of the textures whose content version
/// (or binding) moved since the last render, over what their draws reach (an effect around a draw:
/// the outermost effect's bounds, since a blur or shadow spreads the texture's pixels).

namespace xt {

namespace {

Rect intersectRect(const Rect& a, const Rect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.w, b.x + b.w);
    const float y1 = std::min(a.y + a.h, b.y + b.h);
    return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool positive(const Rect& r) { return r.w > 0.0f && r.h > 0.0f; }

float distance(Vec2 a, Vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

/// A device size in whole pixels (a size computed as 39.9999 is 40).
int wholePixels(float v) {
    if (!(v > 0.0f)) {
        return 0;
    }
    return static_cast<int>(std::min(std::ceil(v - 0.01f), 1.0e9f));
}

}  // namespace

void Context::noteExternalTextures(const CommandList& out, DamageList* damage, bool allSkipped) {
    AssetResolver* assets = m_config.assets;
    // A render that skipped every paint showed the previous list: the previous answers stand.
    if (assets == nullptr || allSkipped) {
        return;
    }
    assets->beginExternalTextureUse();
    bool any = false;
    for (const DocumentHandle handle : m_orderScratch) {
        any = any || !m_documents[handle - 1]->m_externalTextures.empty();
    }
    if (!any) {
        return;
    }
    const float dpr = out.devicePixelRatio > 0.0f ? out.devicePixelRatio : 1.0f;
    const bool damaging = damage != nullptr && !damage->full;
    const Rect device{out.viewport.x * dpr, out.viewport.y * dpr, out.viewport.w * dpr, out.viewport.h * dpr};
    for (DrawnExternal& d : m_drawnExternals) {
        d.seen = false;
    }

    // One external draw: `corners` are its destination's corners on the screen (logical px; `ok` false
    // when the projection failed), `opacity` its own, `uvW` / `uvH` the share of the texture it samples.
    const auto noteDraw = [&](uint16_t source, AssetId asset, const Vec2* corners, size_t count, bool ok,
                              float opacity, float pixelW, float pixelH, Rect clip, float groupOpacity,
                              bool inEffect, const Rect& effectRect) {
        const IrView* ir = source < out.sources.size() ? out.sources[source].ir : nullptr;
        if (ir == nullptr || !isExternalAsset(asset, *ir)) {
            return;
        }
        const std::string_view name = externalAssetName(ir->string(ir->assets().assets()[asset].name));
        const Rect aabb = ok ? pointBounds(std::span<const Vec2>(corners, count)) : clip;
        const Rect shown = intersectRect(aabb, clip);
        if (!(opacity * groupOpacity > 0.0f) || !positive(shown)) {
            return;  // nothing of it reaches the target
        }
        assets->noteExternalTextureUse(name, wholePixels(pixelW), wholePixels(pixelH));
        const AssetResolver::ExternalTexture* ext = assets->externalTexture(name);
        const uint64_t stamp = externalTextureStamp(ext);
        DrawnExternal* drawn = nullptr;
        for (DrawnExternal& d : m_drawnExternals) {
            if (d.name == name) {
                drawn = &d;
                break;
            }
        }
        if (drawn == nullptr) {
            // First drawn now: what the command diff does not see cannot have changed yet.
            drawn = &m_drawnExternals.emplace_back();
            drawn->name = std::string(name);
            drawn->stamp = stamp;
        }
        drawn->seen = true;
        drawn->next = stamp;
        const bool versioned = (ext != nullptr && ext->hasVersion) || drawn->versioned;
        if (!damaging || damage->full || !versioned || drawn->stamp == stamp) {
            return;  // 23 §23.ah item 1: a name without a version reports nothing, as before
        }
        const Rect reach = inEffect ? effectRect : shown;
        const float x0 = std::max(reach.x * dpr, device.x);
        const float y0 = std::max(reach.y * dpr, device.y);
        const float x1 = std::min((reach.x + reach.w) * dpr, device.x + device.w);
        const float y1 = std::min((reach.y + reach.h) * dpr, device.y + device.h);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        damage->rects.push_back(Rect{x0, y0, x1 - x0, y1 - y0});
        if (damage->rects.size() > kMaxDamageRects) {
            damage->rects.clear();
            damage->full = true;
        }
    };

    // World-layer output: every layer is drawn into a target of its own and placed by the host, so
    // only the list's own clips decide what is shown there.
    const Rect unbounded{-1.0e30f, -1.0e30f, 2.0e30f, 2.0e30f};
    const auto walk = [&](size_t first, size_t end, Rect root) {
        m_extClips.assign(1, root);
        m_extOpacity.assign(1, 1.0f);
        m_extMappings.clear();
        int effectDepth = 0;
        Rect effectRect{};
        // A quad draw (DrawImage, DrawImageQuad): `p` its four screen corners, the pixel size along each
        // side pair over the share of the texture (`srcUv`) it samples.
        const auto noteQuad = [&](uint16_t source, AssetId asset, const Vec2* p, bool ok, float opacity,
                                  const Rect& srcUv) {
            const float w = std::max(distance(p[0], p[1]), distance(p[3], p[2])) * dpr /
                            std::max(std::fabs(srcUv.w), 1e-6f);
            const float h = std::max(distance(p[0], p[3]), distance(p[1], p[2])) * dpr /
                            std::max(std::fabs(srcUv.h), 1e-6f);
            noteDraw(source, asset, p, 4, ok, opacity, w, h, m_extClips.back(), m_extOpacity.back(),
                     effectDepth > 0, effectRect);
        };
        const auto map = [&](const Mat2x3& xf, Vec2 p, bool& ok) {
            return m_extMappings.empty() ? apply(xf, p) : project3d::projectPoint(m_extMappings, xf, p, ok);
        };
        for (size_t i = first; i < end && i < out.commands.size(); ++i) {
            const Command& cmd = out.commands[i];
            switch (cmd.kind) {
                case CmdKind::PushClipRect: {
                    const ClipRectCmd& c = out.clipRects[cmd.index];
                    bool ok = true;
                    const Vec2 p[4] = {map(c.xf, Vec2{c.rect.x, c.rect.y}, ok),
                                       map(c.xf, Vec2{c.rect.x + c.rect.w, c.rect.y}, ok),
                                       map(c.xf, Vec2{c.rect.x + c.rect.w, c.rect.y + c.rect.h}, ok),
                                       map(c.xf, Vec2{c.rect.x, c.rect.y + c.rect.h}, ok)};
                    m_extClips.push_back(ok ? intersectRect(m_extClips.back(), pointBounds(p))
                                            : m_extClips.back());
                    break;
                }
                case CmdKind::PushClipPath: {
                    const ClipPathCmd& c = out.clipPaths[cmd.index];
                    bool ok = true;
                    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                    for (uint32_t k = 0; k + 1 < c.path.pointCount; k += 2) {
                        const size_t at = size_t(c.path.pointOffset) + k;
                        if (at + 1 >= out.points.size()) {
                            break;
                        }
                        const Vec2 q = map(c.xf, Vec2{out.points[at], out.points[at + 1]}, ok);
                        x0 = std::min(x0, q.x);
                        y0 = std::min(y0, q.y);
                        x1 = std::max(x1, q.x);
                        y1 = std::max(y1, q.y);
                    }
                    // A path's control points bound its curve; an empty path clips everything away.
                    const Rect box = x1 >= x0 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
                    m_extClips.push_back(ok ? intersectRect(m_extClips.back(), box) : m_extClips.back());
                    break;
                }
                case CmdKind::PopClip:
                    if (m_extClips.size() > 1) {
                        m_extClips.pop_back();
                    }
                    break;
                case CmdKind::PushLayer:
                    m_extOpacity.push_back(m_extOpacity.back() * out.layers[cmd.index].opacity);
                    break;
                case CmdKind::PushMask:
                    m_extOpacity.push_back(m_extOpacity.back());
                    break;
                case CmdKind::PushEffect: {
                    m_extOpacity.push_back(m_extOpacity.back());
                    if (effectDepth++ == 0) {
                        const Rect& hint = out.effectCmds[cmd.index].boundsHint;
                        effectRect =
                            positive(hint) ? intersectRect(hint, m_extClips.back()) : m_extClips.back();
                    }
                    break;
                }
                case CmdKind::PopLayer:
                case CmdKind::PopMask:
                case CmdKind::PopEffect:
                    if (m_extOpacity.size() > 1) {
                        m_extOpacity.pop_back();
                    }
                    if (cmd.kind == CmdKind::PopEffect && effectDepth > 0) {
                        --effectDepth;
                    }
                    break;
                case CmdKind::PushProjection: {
                    const ProjectionCmd& p = out.projections[cmd.index];
                    project3d::Mapping m;
                    m.hasProjection = true;
                    m.viewProj = p.viewProj;
                    m.rootXf = p.rootXf;
                    m_extMappings.push_back(m);
                    break;
                }
                case CmdKind::PopProjection:
                    if (!m_extMappings.empty()) {
                        m_extMappings.pop_back();
                    }
                    break;
                case CmdKind::PushTransform3D:
                    if (!m_extMappings.empty()) {
                        m_extMappings.back().planeWorld3 = out.transforms3[cmd.index].world3;
                    }
                    break;
                case CmdKind::PopTransform3D:
                    if (!m_extMappings.empty()) {
                        m_extMappings.back().planeWorld3 = mat4Identity();
                    }
                    break;
                case CmdKind::DrawImage: {
                    const ImageCmd& c = out.images[cmd.index];
                    bool ok = true;
                    const Vec2 p[4] = {map(c.xf, Vec2{c.dst.x, c.dst.y}, ok),
                                       map(c.xf, Vec2{c.dst.x + c.dst.w, c.dst.y}, ok),
                                       map(c.xf, Vec2{c.dst.x + c.dst.w, c.dst.y + c.dst.h}, ok),
                                       map(c.xf, Vec2{c.dst.x, c.dst.y + c.dst.h}, ok)};
                    noteQuad(c.source, c.asset, p, ok, c.opacity, c.srcUv);
                    break;
                }
                case CmdKind::DrawImageQuad: {
                    const ImageQuadCmd& c = out.imageQuads[cmd.index];
                    bool ok = true;
                    Vec2 p[4];
                    for (int k = 0; k < 4; ++k) {
                        p[k] = map(Mat2x3{}, c.p[k], ok);
                    }
                    noteQuad(c.source, c.asset, p, ok, c.opacity, c.srcUv);
                    break;
                }
                case CmdKind::DrawImageMesh: {
                    const ImageMeshCmd& c = out.imageMeshes[cmd.index];
                    if (c.vertexCount == 0 ||
                        size_t(c.vertexOffset) + c.vertexCount > out.meshVertices.size()) {
                        break;
                    }
                    bool ok = true;
                    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f}, uvLo{1e30f, 1e30f}, uvHi{-1e30f, -1e30f};
                    for (uint32_t k = 0; k < c.vertexCount; ++k) {
                        const MeshVertex& v = out.meshVertices[c.vertexOffset + k];
                        const Vec2 q = map(Mat2x3{}, v.p, ok);
                        lo = Vec2{std::min(lo.x, q.x), std::min(lo.y, q.y)};
                        hi = Vec2{std::max(hi.x, q.x), std::max(hi.y, q.y)};
                        uvLo = Vec2{std::min(uvLo.x, v.uv.x), std::min(uvLo.y, v.uv.y)};
                        uvHi = Vec2{std::max(uvHi.x, v.uv.x), std::max(uvHi.y, v.uv.y)};
                    }
                    const Vec2 p[2] = {lo, hi};
                    noteDraw(c.source, c.asset, p, 2, ok, c.opacity,
                             (hi.x - lo.x) * dpr / std::max(uvHi.x - uvLo.x, 1e-6f),
                             (hi.y - lo.y) * dpr / std::max(uvHi.y - uvLo.y, 1e-6f), m_extClips.back(),
                             m_extOpacity.back(), effectDepth > 0, effectRect);
                    break;
                }
                case CmdKind::DrawParticles: {
                    const ParticlesCmd& c = out.particles[cmd.index];
                    if (c.texture == kInvalidAsset || c.count == 0 ||
                        size_t(c.instanceOffset) + c.count > out.particleInstances.size()) {
                        break;
                    }
                    // Each quad within its centre +- half its larger side times sqrt(2) (any rotation).
                    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
                    float side = 0.0f, uvSpan = 1e-6f;
                    for (uint32_t k = 0; k < c.count; ++k) {
                        const ParticleInstance& q = out.particleInstances[c.instanceOffset + k];
                        const float r = 0.7072f * std::max(q.w, q.h);
                        lo = Vec2{std::min(lo.x, q.x - r), std::min(lo.y, q.y - r)};
                        hi = Vec2{std::max(hi.x, q.x + r), std::max(hi.y, q.y + r)};
                        side = std::max(side, std::max(q.w, q.h));
                        uvSpan = std::max(
                            uvSpan, std::min(std::fabs(q.uv1.x - q.uv0.x), std::fabs(q.uv1.y - q.uv0.y)));
                    }
                    bool ok = true;
                    const Vec2 p[4] = {map(c.xf, lo, ok), map(c.xf, Vec2{hi.x, lo.y}, ok), map(c.xf, hi, ok),
                                       map(c.xf, Vec2{lo.x, hi.y}, ok)};
                    const float scale = std::sqrt(std::fabs(c.xf.a * c.xf.d - c.xf.b * c.xf.c));
                    const float size = side * scale * dpr / uvSpan;
                    noteDraw(c.source, c.texture, p, 4, ok, 1.0f, size, size, m_extClips.back(),
                             m_extOpacity.back(), effectDepth > 0, effectRect);
                    break;
                }
                default:
                    break;
            }
        }
    };
    if (out.worldLayers.empty()) {
        walk(0, out.commands.size(), out.viewport);
    } else {
        for (const WorldLayerRange& range : out.worldLayers) {
            walk(range.firstCommand, size_t(range.firstCommand) + range.commandCount, unbounded);
        }
    }
    for (DrawnExternal& d : m_drawnExternals) {
        if (d.seen) {
            d.stamp = d.next;
            const AssetResolver::ExternalTexture* ext = assets->externalTexture(d.name);
            d.versioned = ext != nullptr && ext->hasVersion;
        }
    }
}

}  // namespace xt

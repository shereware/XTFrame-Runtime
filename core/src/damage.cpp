#include <algorithm>
#include <cmath>
#include <cstring>

#include "damage_internal.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"

/// Damage (dirty rectangle) computation from a command-list diff. See
/// docs/design/23-host-integration.md §23.2, docs/tasks/T-98.md.

namespace xt {

void DamageList::clear() {
    rects.clear();
    full = false;
}

Rect DamageList::bounds() const {
    if (full) {
        return viewport;
    }
    Rect result{};
    for (const Rect& r : rects) {
        result = unite(result, r);
    }
    return result;
}

namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

/// FNV-1a 64 over `count` bytes of `data`, continuing from `hash`.
uint64_t hashBytes(uint64_t hash, const void* data, size_t count) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

/// Hashes one POD payload. The payloads are value-initialized by Painter, so their padding bytes
/// are zero and hashing the whole object is stable (docs/tasks/T-98.md "命令雜湊").
template <class T>
uint64_t hashPod(uint64_t hash, const T& value) {
    return hashBytes(hash, &value, sizeof(T));
}

/// Hashes the `count` pool entries starting at `offset`, skipping a range that runs off the pool
/// (a malformed list never makes this read out of bounds).
template <class T>
uint64_t hashPool(uint64_t hash, const std::vector<T>& pool, size_t offset, size_t count) {
    if (count == 0 || offset > pool.size() || pool.size() - offset < count) {
        return hash;
    }
    return hashBytes(hash, pool.data() + offset, count * sizeof(T));
}

/// Hashes a path's verb and point ranges.
uint64_t hashPath(uint64_t hash, const CommandList& list, const PathRef& path) {
    hash = hashPool(hash, list.verbs, path.verbOffset, path.verbCount);
    return hashPool(hash, list.points, path.pointOffset, path.pointCount);
}

/// AABB of `rect`'s four corners after `xf`.
Rect transformedRect(const Mat2x3& xf, const Rect& rect) {
    Vec2 corners[4] = {
        apply(xf, Vec2{rect.x, rect.y}),
        apply(xf, Vec2{rect.x + rect.w, rect.y}),
        apply(xf, Vec2{rect.x + rect.w, rect.y + rect.h}),
        apply(xf, Vec2{rect.x, rect.y + rect.h}),
    };
    float minX = corners[0].x;
    float minY = corners[0].y;
    float maxX = minX;
    float maxY = minY;
    for (size_t i = 1; i < 4; ++i) {
        minX = std::min(minX, corners[i].x);
        minY = std::min(minY, corners[i].y);
        maxX = std::max(maxX, corners[i].x);
        maxY = std::max(maxY, corners[i].y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

/// AABB of a path's raw control points after `xf` (conservative: control points bound their
/// curves).
Rect transformedPath(const CommandList& list, const Mat2x3& xf, const PathRef& path) {
    if (path.pointCount < 2 || path.pointOffset + path.pointCount > list.points.size()) {
        return Rect{};
    }
    bool any = false;
    float minX = 0;
    float minY = 0;
    float maxX = 0;
    float maxY = 0;
    // CommandList::points holds two floats per point, and PathRef::pointCount counts floats.
    for (uint32_t i = 0; i + 1 < path.pointCount; i += 2) {
        Vec2 p = apply(xf, Vec2{list.points[path.pointOffset + i], list.points[path.pointOffset + i + 1]});
        if (!any) {
            minX = maxX = p.x;
            minY = maxY = p.y;
            any = true;
            continue;
        }
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    if (!any) {
        return Rect{};
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

/// True when `a` and `b` overlap (touching edges do not count).
bool rectsIntersect(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/// Outsets `rect` by `amount` on every side.
Rect outset(const Rect& rect, float amount) {
    return Rect{rect.x - amount, rect.y - amount, rect.w + 2 * amount, rect.h + 2 * amount};
}

/// Largest length scale `xf` can apply to a direction vector (the larger column norm bounds the
/// matrix's spectral norm closely enough for a conservative stroke outset).
float maxScale(const Mat2x3& xf) {
    float col0 = std::sqrt(xf.a * xf.a + xf.b * xf.b);
    float col1 = std::sqrt(xf.c * xf.c + xf.d * xf.d);
    return std::max(col0, col1);
}

/// Area of `rect`, 0 when empty.
float areaOf(const Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) {
        return 0;
    }
    return rect.w * rect.h;
}

/// Clips `rect` to `viewport` and snaps it outwards to whole device pixels. An empty result means
/// "nothing to add".
Rect clipAndSnap(const Rect& rect, const Rect& viewport) {
    if (rect.w <= 0 || rect.h <= 0) {
        return Rect{};
    }
    float x0 = std::max(std::floor(rect.x), viewport.x);
    float y0 = std::max(std::floor(rect.y), viewport.y);
    float x1 = std::min(std::ceil(rect.x + rect.w), viewport.x + viewport.w);
    float y1 = std::min(std::ceil(rect.y + rect.h), viewport.y + viewport.h);
    if (x1 <= x0 || y1 <= y0) {
        return Rect{};
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// True when `inner` lies entirely within `outer`.
bool rectContains(const Rect& outer, const Rect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

/// §23.2 step 4's merge, run incrementally: `rects` never grows past kMaxDamageRects, because the
/// pair whose union adds the least area is merged as soon as it would. Incremental keeps the cost
/// at O(kMaxDamageRects^2) per added rectangle instead of O(n^3) for n changed commands.
void addRect(std::vector<Rect>& rects, const Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }
    // A command whose payload changed but whose geometry did not (a color change, say) yields the
    // same rectangle from both the old and the new range; nested commands yield rectangles inside
    // their parent's. Swallowing those keeps the reported set minimal before any merging.
    for (size_t i = 0; i < rects.size(); ++i) {
        if (rectContains(rects[i], rect)) {
            return;
        }
    }
    for (size_t i = rects.size(); i-- > 0;) {
        if (rectContains(rect, rects[i])) {
            rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    rects.push_back(rect);
    while (rects.size() > kMaxDamageRects) {
        size_t bestI = 0;
        size_t bestJ = 1;
        float bestGrowth = 0;
        bool first = true;
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                float growth = areaOf(unite(rects[i], rects[j])) - areaOf(rects[i]) - areaOf(rects[j]);
                if (first || growth < bestGrowth) {
                    bestGrowth = growth;
                    bestI = i;
                    bestJ = j;
                    first = false;
                }
            }
        }
        rects[bestI] = unite(rects[bestI], rects[bestJ]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(bestJ));
    }
}

}  // namespace

void hashCommandList(const CommandList& list, std::vector<uint64_t>& out) {
    out.clear();
    out.reserve(list.commands.size());
    for (const Command& cmd : list.commands) {
        uint8_t kind = static_cast<uint8_t>(cmd.kind);
        uint64_t hash = hashBytes(kFnvOffsetBasis, &kind, 1);
        switch (cmd.kind) {
            case CmdKind::FillPath: {
                const FillCmd& c = list.fills[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPath(hash, list, c.path);
                hash = hashPool(hash, list.stops, c.paint.stopOffset, c.paint.stopCount);
                break;
            }
            case CmdKind::StrokePath: {
                const StrokeCmd& c = list.strokes[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPath(hash, list, c.path);
                hash = hashPool(hash, list.stops, c.paint.stopOffset, c.paint.stopCount);
                hash = hashPool(hash, list.dashes, c.dashOffset, c.dashCount);
                break;
            }
            case CmdKind::DrawImage:
                hash = hashPod(hash, list.images[cmd.index]);
                break;
            case CmdKind::DrawGlyphs: {
                const GlyphsCmd& c = list.glyphRuns[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPool(hash, list.glyphs, c.glyphOffset, c.glyphCount);
                hash = hashPool(hash, list.glyphPos, c.glyphOffset, c.glyphCount);
                break;
            }
            case CmdKind::PushClipRect:
                hash = hashPod(hash, list.clipRects[cmd.index]);
                break;
            case CmdKind::PushClipPath: {
                const ClipPathCmd& c = list.clipPaths[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPath(hash, list, c.path);
                break;
            }
            case CmdKind::PushLayer:
                hash = hashPod(hash, list.layers[cmd.index]);
                break;
            case CmdKind::PushMask:
                hash = hashPod(hash, list.masks[cmd.index]);
                break;
            case CmdKind::PushProjection:
                hash = hashPod(hash, list.projections[cmd.index]);
                break;
            case CmdKind::PushTransform3D:
                hash = hashPod(hash, list.transforms3[cmd.index]);
                break;
            case CmdKind::DrawImageQuad:
                hash = hashPod(hash, list.imageQuads[cmd.index]);
                break;
            case CmdKind::PushEffect: {
                const EffectCmd& c = list.effectCmds[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPool(hash, list.effects, c.effectOffset, c.effectCount);
                break;
            }
            case CmdKind::DrawParticles: {
                const ParticlesCmd& c = list.particles[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPool(hash, list.particleInstances, c.instanceOffset, c.count);
                break;
            }
            case CmdKind::DrawImageMesh: {
                const ImageMeshCmd& c = list.imageMeshes[cmd.index];
                hash = hashPod(hash, c);
                hash = hashPool(hash, list.meshVertices, c.vertexOffset, c.vertexCount);
                hash = hashPool(hash, list.meshIndices, c.indexOffset, c.indexCount);
                break;
            }
            case CmdKind::PopClip:
            case CmdKind::PopLayer:
            case CmdKind::EndMaskContent:
            case CmdKind::PopMask:
            case CmdKind::PopProjection:
            case CmdKind::PopTransform3D:
            case CmdKind::PopEffect:
                break;  // No payload: the kind byte alone identifies the command.
        }
        out.push_back(hash);
    }
}

Rect commandBounds(const CommandList& list, const Command& cmd) {
    switch (cmd.kind) {
        case CmdKind::FillPath: {
            const FillCmd& c = list.fills[cmd.index];
            return transformedPath(list, c.xf, c.path);
        }
        case CmdKind::StrokePath: {
            const StrokeCmd& c = list.strokes[cmd.index];
            Rect box = transformedPath(list, c.xf, c.path);
            if (box.w <= 0 && box.h <= 0) {
                return box;
            }
            // B7 (docs/tasks/T-263a.md): miter tips and square caps reach past width / 2; the
            // same reach the Painter's node bounds use.
            return outset(box, strokeOutset(c.width, c.cap, c.join, c.miterLimit) * maxScale(c.xf));
        }
        case CmdKind::DrawImage: {
            const ImageCmd& c = list.images[cmd.index];
            return transformedRect(c.xf, c.dst);
        }
        case CmdKind::DrawGlyphs: {
            // Coarse box: the baseline origins outset by the font size on every side bounds the
            // run's ascent/descent and its first/last glyph's side bearings (T-98's "粗略框").
            const GlyphsCmd& c = list.glyphRuns[cmd.index];
            if (c.glyphCount == 0 || c.glyphOffset + c.glyphCount > list.glyphPos.size()) {
                return Rect{};
            }
            Vec2 first = list.glyphPos[c.glyphOffset];
            float minX = first.x;
            float minY = first.y;
            float maxX = minX;
            float maxY = minY;
            for (uint32_t i = 1; i < c.glyphCount; ++i) {
                Vec2 p = list.glyphPos[c.glyphOffset + i];
                minX = std::min(minX, p.x);
                minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x);
                maxY = std::max(maxY, p.y);
            }
            // T-178: a glyph effect (dilate R / blur 3 sigma = 1.5 R) reaches at most 2 R further.
            Rect box = outset(Rect{minX, minY, maxX - minX, maxY - minY}, c.size + 2.0f * c.effectRadius);
            return transformedRect(c.xf, box);
        }
        case CmdKind::PushClipRect: {
            const ClipRectCmd& c = list.clipRects[cmd.index];
            return transformedRect(c.xf, c.rect);
        }
        case CmdKind::PushClipPath: {
            const ClipPathCmd& c = list.clipPaths[cmd.index];
            return transformedPath(list, c.xf, c.path);
        }
        case CmdKind::PushLayer:
            return list.layers[cmd.index].boundsHint;
        case CmdKind::PushMask:
            return list.masks[cmd.index].boundsHint;
        case CmdKind::PushEffect:
            // EffectCmd::boundsHint is already outset by the stacked effects' expansion
            // (effectExpand(), docs/design/18-effects.md §18.3) and clipped to the viewport.
            return list.effectCmds[cmd.index].boundsHint;
        case CmdKind::PushProjection:
            // ProjectionCmd::boundsHint is the projected subtree's screen AABB, already outset by
            // the subtree's largest effect expansion and clipped to the viewport (T-105,
            // docs/design/19-3d-space.md §19.6 step 1); it is the whole viewport when the Painter
            // could not bound the subtree.
            return list.projections[cmd.index].boundsHint;
        case CmdKind::DrawImageQuad: {
            const ImageQuadCmd& c = list.imageQuads[cmd.index];
            float minX = c.p[0].x;
            float minY = c.p[0].y;
            float maxX = minX;
            float maxY = minY;
            for (size_t i = 1; i < 4; ++i) {
                minX = std::min(minX, c.p[i].x);
                minY = std::min(minY, c.p[i].y);
                maxX = std::max(maxX, c.p[i].x);
                maxY = std::max(maxY, c.p[i].y);
            }
            return Rect{minX, minY, maxX - minX, maxY - minY};
        }
        case CmdKind::DrawParticles: {
            // T-202: every instance's rotated quad fits in the square of its half-diagonal.
            const ParticlesCmd& c = list.particles[cmd.index];
            bool any = false;
            Rect box{};
            for (uint32_t i = 0; i < c.count; ++i) {
                const ParticleInstance& p = list.particleInstances[c.instanceOffset + i];
                const float r = 0.5f * std::sqrt(p.w * p.w + p.h * p.h);
                const Rect q = transformedRect(c.xf, Rect{p.x - r, p.y - r, 2 * r, 2 * r});
                if (!any) {
                    box = q;
                    any = true;
                } else {
                    const float x0 = std::min(box.x, q.x);
                    const float y0 = std::min(box.y, q.y);
                    const float x1 = std::max(box.x + box.w, q.x + q.w);
                    const float y1 = std::max(box.y + box.h, q.y + q.h);
                    box = Rect{x0, y0, x1 - x0, y1 - y0};
                }
            }
            return box;
        }
        case CmdKind::DrawImageMesh: {
            // T-245: the AABB of the mesh's vertices (already in screen/root-box coordinates).
            const ImageMeshCmd& c = list.imageMeshes[cmd.index];
            if (c.vertexCount == 0 ||
                static_cast<size_t>(c.vertexOffset) + c.vertexCount > list.meshVertices.size()) {
                return Rect{};
            }
            Vec2 first = list.meshVertices[c.vertexOffset].p;
            float minX = first.x;
            float minY = first.y;
            float maxX = minX;
            float maxY = minY;
            for (uint32_t i = 1; i < c.vertexCount; ++i) {
                const Vec2 p = list.meshVertices[c.vertexOffset + i].p;
                minX = std::min(minX, p.x);
                minY = std::min(minY, p.y);
                maxX = std::max(maxX, p.x);
                maxY = std::max(maxY, p.y);
            }
            return Rect{minX, minY, maxX - minX, maxY - minY};
        }
        case CmdKind::PushTransform3D:
        case CmdKind::PopClip:
        case CmdKind::PopLayer:
        case CmdKind::EndMaskContent:
        case CmdKind::PopMask:
        case CmdKind::PopProjection:
        case CmdKind::PopTransform3D:
        case CmdKind::PopEffect:
            return Rect{};  // Draws nothing by itself; the matching Push carries the bounds.
    }
    return Rect{};
}

void computeDamage(const CommandList& prev, const CommandList& cur, Rect viewportDevice, DamageList& out) {
    out.clear();
    out.viewport = viewportDevice;

    // §23.2 step 2: hash both lists, then find the first and the last differing command.
    static thread_local std::vector<uint64_t> prevHashes;
    static thread_local std::vector<uint64_t> curHashes;
    /// Indices of the PushEffect commands open at the start of a changed range (B6 below).
    static thread_local std::vector<size_t> openEffects;
    hashCommandList(prev, prevHashes);
    hashCommandList(cur, curHashes);

    size_t prevCount = prevHashes.size();
    size_t curCount = curHashes.size();
    size_t common = std::min(prevCount, curCount);
    size_t head = 0;
    while (head < common && prevHashes[head] == curHashes[head]) {
        ++head;
    }
    size_t tail = 0;
    while (tail < common - head && prevHashes[prevCount - 1 - tail] == curHashes[curCount - 1 - tail]) {
        ++tail;
    }
    size_t prevEnd = prevCount - tail;
    size_t curEnd = curCount - tail;
    // T-203 (docs/design/18-effects.md §18.7): a custom effect's shader may read `xtTime`, so
    // its node changes on screen whenever the clock moves even though no command did.
    const bool clockMoved = !cur.shaders.empty() && cur.time != prev.time;
    if (head >= prevEnd && head >= curEnd && !clockMoved) {
        return;  // Identical lists: nothing changed on screen.
    }

    // §23.2 step 3: the changed ranges' bounds, in device pixels, clipped to the viewport.
    float dpr = cur.devicePixelRatio > 0 ? cur.devicePixelRatio : 1.0f;
    /// T-332: the bounding boxes of the PushClipPath commands open at the current command, each
    /// already intersected with the ones outside it (a PushClipRect pushes its enclosing entry
    /// unchanged, so overflow clips keep their pre-T-332 behaviour).
    static thread_local std::vector<Rect> openClips;
    auto collect = [&](const CommandList& list, size_t from, size_t to, float listDpr) {
        auto add = [&](const Rect& box) {
            Rect device{box.x * listDpr, box.y * listDpr, box.w * listDpr, box.h * listDpr};
            addRect(out.rects, clipAndSnap(device, viewportDevice));
        };
        // T-332 (05 §5.9.8): a command inside a `clip-path` can only change pixels inside the
        // shape's bounding box, so its damage is its bounds ∩ the open clip paths' boxes (the
        // clip commands themselves included, nested ones cut by the outer).
        openClips.clear();
        auto trackClip = [&](const Command& cmd, Rect bounds) {
            switch (cmd.kind) {
                case CmdKind::PushClipPath:
                    openClips.push_back(bounds);
                    break;
                case CmdKind::PushClipRect:
                    openClips.push_back(openClips.empty() ? Rect{-1e30f, -1e30f, 2e30f, 2e30f}
                                                          : openClips.back());
                    break;
                case CmdKind::PopClip:
                    if (!openClips.empty()) {
                        openClips.pop_back();
                    }
                    break;
                default:
                    break;
            }
        };
        auto clipped = [&](Rect bounds) {
            return openClips.empty() ? bounds : intersect(bounds, openClips.back());
        };
        for (size_t i = 0; i < from; ++i) {
            const Command& cmd = list.commands[i];
            if (cmd.kind == CmdKind::PushClipPath || cmd.kind == CmdKind::PushClipRect ||
                cmd.kind == CmdKind::PopClip) {
                trackClip(cmd, clipped(commandBounds(list, cmd)));
            }
        }
        for (size_t i = from; i < to; ++i) {
            const Command& cmd = list.commands[i];
            const Rect bounds = clipped(commandBounds(list, cmd));
            add(bounds);
            trackClip(cmd, bounds);
        }
        if (from >= to) {
            return;
        }
        // B6 (docs/tasks/T-263a.md): an effect whose PushEffect is still open where the change
        // starts encloses it, and its halo (a glow, a shadow, a blur) follows the content, so its
        // boundsHint -- already outset by the stack's expansion -- is repainted too. Effects
        // opened inside the range are covered by their own PushEffect above.
        openEffects.clear();
        for (size_t i = 0; i < from; ++i) {
            const CmdKind kind = list.commands[i].kind;
            if (kind == CmdKind::PushEffect) {
                openEffects.push_back(i);
            } else if (kind == CmdKind::PopEffect && !openEffects.empty()) {
                openEffects.pop_back();
            }
        }
        for (size_t i : openEffects) {
            add(commandBounds(list, list.commands[i]));
        }
    };
    float prevDpr = prev.devicePixelRatio > 0 ? prev.devicePixelRatio : 1.0f;
    collect(prev, head, prevEnd, prevDpr);
    collect(cur, head, curEnd, dpr);
    if (clockMoved) {
        for (const Command& cmd : cur.commands) {
            if (cmd.kind != CmdKind::PushEffect) {
                continue;
            }
            const EffectCmd& c = cur.effectCmds[cmd.index];
            for (uint32_t i = 0; i < c.effectCount && c.effectOffset + i < cur.effects.size(); ++i) {
                if (cur.effects[c.effectOffset + i].kind == EffectKind::Custom) {
                    Rect device{c.boundsHint.x * dpr, c.boundsHint.y * dpr, c.boundsHint.w * dpr,
                                c.boundsHint.h * dpr};
                    addRect(out.rects, clipAndSnap(device, viewportDevice));
                    break;
                }
            }
        }
    }

    // docs/design/18-effects.md §18.6 (T-130): a BackdropBlur node reads what was drawn below it,
    // so anything changing under it changes its own pixels even when none of its own commands
    // did. Conservatively: every BackdropBlur PushEffect whose *read* rectangle (its boundsHint
    // outset by effectReadExpand()'s 3 sigma) meets a dirty rectangle joins the damage. Repeated
    // until stable, because adding one such rectangle can bring a second glass panel (one stacked
    // over another) into the dirty set; the loop runs at most once per PushEffect command.
    size_t backdropRounds = 0;
    for (const Command& cmd : cur.commands) {
        backdropRounds += (cmd.kind == CmdKind::PushEffect) ? 1 : 0;
    }
    for (size_t round = 0; round < backdropRounds && !out.rects.empty(); ++round) {
        bool added = false;
        for (const Command& cmd : cur.commands) {
            if (cmd.kind != CmdKind::PushEffect) {
                continue;
            }
            const EffectCmd& c = cur.effectCmds[cmd.index];
            if (c.effectCount == 0 || c.effectOffset >= cur.effects.size()) {
                continue;
            }
            const EffectInstance& first = cur.effects[c.effectOffset];
            if (first.kind != EffectKind::BackdropBlur || first.enabled == 0) {
                continue;
            }
            float readExpand = effectReadExpand(effectToScreen(first, c.xf));
            Rect read = outset(c.boundsHint, readExpand);
            Rect device{read.x * dpr, read.y * dpr, read.w * dpr, read.h * dpr};
            Rect snapped = clipAndSnap(device, viewportDevice);
            if (snapped.w <= 0 || snapped.h <= 0) {
                continue;
            }
            bool covered = false;
            bool touches = false;
            for (const Rect& r : out.rects) {
                covered = covered || rectContains(r, snapped);
                touches = touches || rectsIntersect(r, snapped);
            }
            if (!touches || covered) {
                continue;
            }
            addRect(out.rects, snapped);
            added = true;
        }
        if (!added) {
            break;
        }
    }

    // docs/design/18-effects.md §18.12 (T-326): a Layer composited with a non-Normal blend mode
    // (an element's `mix-blend-mode`, or a scene node's blend) mixes its content with whatever
    // was drawn below it, so a change underneath changes its pixels too -- the BackdropBlur rule
    // above, applied to the Layer's boundsHint. A change inside the layer needs nothing extra:
    // its own commands' rectangles are already dirty. Repeated like the BackdropBlur loop so a
    // blended layer brought into the dirty set can bring a second one stacked over it.
    size_t blendRounds = 0;
    for (const Command& cmd : cur.commands) {
        blendRounds += (cmd.kind == CmdKind::PushLayer) ? 1 : 0;
    }
    for (size_t round = 0; round < blendRounds && !out.rects.empty(); ++round) {
        bool added = false;
        for (const Command& cmd : cur.commands) {
            if (cmd.kind != CmdKind::PushLayer || cmd.index >= cur.layers.size()) {
                continue;
            }
            const LayerCmd& layer = cur.layers[cmd.index];
            if (layer.blend == BlendMode::Normal) {
                continue;
            }
            const Rect& b = layer.boundsHint;
            Rect device{b.x * dpr, b.y * dpr, b.w * dpr, b.h * dpr};
            Rect snapped = clipAndSnap(device, viewportDevice);
            if (snapped.w <= 0 || snapped.h <= 0) {
                continue;
            }
            bool covered = false;
            bool touches = false;
            for (const Rect& r : out.rects) {
                covered = covered || rectContains(r, snapped);
                touches = touches || rectsIntersect(r, snapped);
            }
            if (!touches || covered) {
                continue;
            }
            addRect(out.rects, snapped);
            added = true;
        }
        if (!added) {
            break;
        }
    }

    // §23.2 step 4: too much area left after merging -> just repaint everything.
    float total = 0;
    for (const Rect& r : out.rects) {
        total += areaOf(r);
    }
    float viewportArea = areaOf(viewportDevice);
    if (out.rects.empty()) {
        return;
    }
    if (viewportArea <= 0 || total > kFullDamageAreaRatio * viewportArea) {
        out.rects.clear();
        out.full = true;
    }
}

}  // namespace xt

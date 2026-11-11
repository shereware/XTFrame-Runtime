#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "damage_internal.h"
#include "xt/box_shadow.h"
#include "xt/effect_utils.h"
#include "xt/geom.h"
#include "xt/text_shaper.h"

/// Damage (dirty rectangle) computation from a command-list diff. See
/// Docs/design/23-host-integration.md §23.2, Docs/tasks/T-98.md.

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

/// Hashes one POD payload as its raw bytes (Docs/tasks/T-98.md "命令雜湊"). Only for types without
/// padding: padding bytes are not guaranteed to survive value-initialization or a copy (Clang leaves
/// them indeterminate), so two equal payloads would hash differently. The padded payloads go
/// through hashFields() below instead (T-402).
template <class T>
uint64_t hashPod(uint64_t hash, const T& value) {
    return hashBytes(hash, &value, sizeof(T));
}

/// T-402: the payloads with padding (PaintRef after `kind`, FillCmd's tail, StrokeCmd after
/// `join`, GlyphsCmd after `rendering`, ClipPathCmd's tail, LayerCmd after `blend`, MaskCmd after
/// `mode`) are hashed member by member, each member being padding-free. The size checks make a new
/// member fail the build here until it is added to the matching overload.
static_assert(sizeof(PaintRef) == 52 && sizeof(FillCmd) == 96 && sizeof(StrokeCmd) == 116 &&
                  sizeof(GlyphsCmd) == 68 && sizeof(ClipPathCmd) == 44 && sizeof(LayerCmd) == 24 &&
                  sizeof(MaskCmd) == 20,
              "a padded command payload changed: update its hashFields() overload");

/// T-644: pool offsets (PathRef's verb/point offsets, stopOffset, dashOffset, glyphOffset,
/// effectOffset, instanceOffset, vertexOffset, indexOffset) are left out of every hash: the pool
/// entries they point at are hashed by content, and an inserted or deleted command shifts the offsets
/// of every command after it, which would make the whole rest of the list look changed to the
/// sequence diff of §23.2 step 2.
uint64_t hashFields(uint64_t hash, const PathRef& path) {
    hash = hashPod(hash, path.verbCount);
    return hashPod(hash, path.pointCount);
}

uint64_t hashFields(uint64_t hash, const PaintRef& p) {
    hash = hashPod(hash, p.kind);
    hash = hashPod(hash, p.color);
    hash = hashPod(hash, p.p0);
    hash = hashPod(hash, p.p1);
    hash = hashPod(hash, p.p2);
    return hashPod(hash, p.stopCount);
}

uint64_t hashFields(uint64_t hash, const FillCmd& c) {
    hash = hashPod(hash, c.xf);
    hash = hashFields(hash, c.path);
    hash = hashFields(hash, c.paint);
    hash = hashPod(hash, c.rule);
    return hashPod(hash, c.antialias);
}

uint64_t hashFields(uint64_t hash, const StrokeCmd& c) {
    hash = hashPod(hash, c.xf);
    hash = hashFields(hash, c.path);
    hash = hashFields(hash, c.paint);
    hash = hashPod(hash, c.width);
    hash = hashPod(hash, c.cap);
    hash = hashPod(hash, c.join);
    hash = hashPod(hash, c.miterLimit);
    hash = hashPod(hash, c.dashCount);
    return hashPod(hash, c.dashPhase);
}

uint64_t hashFields(uint64_t hash, const GlyphsCmd& c) {
    hash = hashPod(hash, c.xf);
    hash = hashPod(hash, c.font);
    hash = hashPod(hash, c.size);
    hash = hashPod(hash, c.color);
    hash = hashPod(hash, c.rendering);
    hash = hashPod(hash, c.glyphCount);
    hash = hashPod(hash, c.effect);
    hash = hashPod(hash, c._pad);
    return hashPod(hash, c.effectRadius);
}

uint64_t hashFields(uint64_t hash, const ClipPathCmd& c) {
    hash = hashPod(hash, c.xf);
    hash = hashFields(hash, c.path);
    return hashPod(hash, c.rule);
}

uint64_t hashFields(uint64_t hash, const LayerCmd& c) {
    hash = hashPod(hash, c.opacity);
    hash = hashPod(hash, c.blend);
    return hashPod(hash, c.boundsHint);
}

uint64_t hashFields(uint64_t hash, const MaskCmd& c) {
    hash = hashPod(hash, c.mode);
    return hashPod(hash, c.boundsHint);
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

/// A glyph run's coarse box before its `xf`: the baseline origins outset by the font size on every
/// side bounds the run's ascent/descent and its first/last glyph's side bearings (T-98's "粗略框");
/// T-178: a glyph effect (dilate R / blur 3 sigma = 1.5 R) reaches at most 2 R further. False for an
/// empty or out-of-range run.
bool glyphRunBox(const CommandList& list, const GlyphsCmd& c, Rect& box) {
    if (c.glyphCount == 0 || c.glyphOffset + c.glyphCount > list.glyphPos.size()) {
        return false;
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
    box = outset(Rect{minX, minY, maxX - minX, maxY - minY}, c.size + 2.0f * c.effectRadius);
    return true;
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

/// T-644: a group opener whose commandBounds() also bound every pixel its content can reach (the
/// clip shapes, and the boundsHint of a layer, mask, effect or projection, whose buffers are that
/// big). PushTransform3D draws nothing by itself but only occurs inside a PushProjection, whose
/// boundsHint covers its content (a changed transform alone added nothing before T-644 either).
/// EndMaskContent has no payload: it is unmatched only when it moved, i.e. the mask's phases changed.
bool boundsItsContent(CmdKind kind) {
    switch (kind) {
        case CmdKind::PushClipRect:
        case CmdKind::PushClipPath:
        case CmdKind::PushLayer:
        case CmdKind::PushMask:
        case CmdKind::PushEffect:
        case CmdKind::PushProjection:
        case CmdKind::PushTransform3D:
            return true;
        default:
            return false;
    }
}

/// T-644: keeps `open` the stack of the group commands enclosing the command after `index` (every
/// Push*, plus EndMaskContent as its own level so the mask's two phases differ).
void trackOpenGroups(const CommandList& list, size_t index, std::vector<uint32_t>& open) {
    switch (list.commands[index].kind) {
        case CmdKind::PushClipRect:
        case CmdKind::PushClipPath:
        case CmdKind::PushLayer:
        case CmdKind::PushMask:
        case CmdKind::PushProjection:
        case CmdKind::PushTransform3D:
        case CmdKind::PushEffect:
        case CmdKind::EndMaskContent:
            open.push_back(static_cast<uint32_t>(index));
            break;
        case CmdKind::PopMask:
            if (!open.empty() && list.commands[open.back()].kind == CmdKind::EndMaskContent) {
                open.pop_back();
            }
            [[fallthrough]];
        case CmdKind::PopClip:
        case CmdKind::PopLayer:
        case CmdKind::PopProjection:
        case CmdKind::PopTransform3D:
        case CmdKind::PopEffect:
            if (!open.empty()) {
                open.pop_back();
            }
            break;
        default:
            break;
    }
}

/// T-644: whether a matched pair of commands draws in the same context. Level by level, the
/// enclosing groups must be matched with each other, or both be unmatched (changed) groups that bound
/// their content -- then both groups' bounds are dirty and cover whatever the move between them
/// changes, as a group whose payload changed in place always did.
bool sameContext(const CommandList& prev, const CommandList& cur, const std::vector<uint32_t>& prevOpen,
                 const std::vector<uint32_t>& curOpen, const std::vector<uint32_t>& prevToCur,
                 const std::vector<uint32_t>& curToPrev) {
    if (prevOpen.size() != curOpen.size()) {
        return false;
    }
    for (size_t level = 0; level < prevOpen.size(); ++level) {
        const uint32_t p = prevOpen[level];
        const uint32_t c = curOpen[level];
        if (prevToCur[p] == c) {
            continue;
        }
        if (prevToCur[p] == kNoCommandMatch && curToPrev[c] == kNoCommandMatch &&
            boundsItsContent(prev.commands[p].kind) && boundsItsContent(cur.commands[c].kind)) {
            continue;
        }
        return false;
    }
    return true;
}

/// T-644: the dirty commands of both lists: every unmatched one, plus every matched pair whose
/// enclosing groups differ (sameContext()) -- a matched command moved into, out of or between groups
/// can change pixels its unchanged hash does not show (a clip wrapped around it, say).
void markDirty(const CommandList& prev, const CommandList& cur, const std::vector<uint32_t>& prevToCur,
               const std::vector<uint32_t>& curToPrev, std::vector<uint8_t>& prevDirty,
               std::vector<uint8_t>& curDirty) {
    static thread_local std::vector<uint32_t> prevOpen;
    static thread_local std::vector<uint32_t> curOpen;
    const size_t prevCount = prev.commands.size();
    const size_t curCount = cur.commands.size();
    prevDirty.resize(prevCount);
    curDirty.resize(curCount);
    for (size_t i = 0; i < prevCount; ++i) {
        prevDirty[i] = prevToCur[i] == kNoCommandMatch ? 1 : 0;
    }
    for (size_t j = 0; j < curCount; ++j) {
        curDirty[j] = curToPrev[j] == kNoCommandMatch ? 1 : 0;
    }
    prevOpen.clear();
    curOpen.clear();
    size_t i = 0;
    size_t j = 0;
    while (i < prevCount || j < curCount) {
        if (i < prevCount && prevToCur[i] == kNoCommandMatch) {
            trackOpenGroups(prev, i++, prevOpen);
            continue;
        }
        if (j < curCount && curToPrev[j] == kNoCommandMatch) {
            trackOpenGroups(cur, j++, curOpen);
            continue;
        }
        if (i >= prevCount || j >= curCount) {
            break;  // Unreachable: matches pair up in order.
        }
        if (!sameContext(prev, cur, prevOpen, curOpen, prevToCur, curToPrev)) {
            prevDirty[i] = 1;
            curDirty[j] = 1;
        }
        trackOpenGroups(prev, i++, prevOpen);
        trackOpenGroups(cur, j++, curOpen);
    }
}

}  // namespace

bool matchCommandHashes(const std::vector<uint64_t>& prev, const std::vector<uint64_t>& cur,
                        std::vector<uint32_t>& prevToCur, std::vector<uint32_t>& curToPrev) {
    const size_t prevCount = prev.size();
    const size_t curCount = cur.size();
    prevToCur.assign(prevCount, kNoCommandMatch);
    curToPrev.assign(curCount, kNoCommandMatch);
    auto link = [&](size_t i, size_t j) {
        prevToCur[i] = static_cast<uint32_t>(j);
        curToPrev[j] = static_cast<uint32_t>(i);
    };
    const size_t common = std::min(prevCount, curCount);
    size_t head = 0;
    while (head < common && prev[head] == cur[head]) {
        link(head, head);
        ++head;
    }
    size_t tail = 0;
    while (tail < common - head && prev[prevCount - 1 - tail] == cur[curCount - 1 - tail]) {
        link(prevCount - 1 - tail, curCount - 1 - tail);
        ++tail;
    }
    const size_t n = prevCount - head - tail;
    const size_t m = curCount - head - tail;
    if (n == 0 || m == 0) {
        return true;  // Only insertions or only deletions: head and tail are already the alignment.
    }
    if (std::max(prevCount, curCount) > kDamageDiffMaxCommands) {
        return false;
    }

    // Myers' greedy forward search over the middle a = prev[head, head + n), b = cur[head, head + m):
    // after d edits, v[k] is the furthest x reached on diagonal k = x - y. Every step's row
    // (k = -d..d) is kept for the backtrack: row d starts at trace[d * d].
    using Index = std::ptrdiff_t;
    const Index n0 = static_cast<Index>(n);
    const Index m0 = static_cast<Index>(m);
    const Index maxD = static_cast<Index>(std::min(n + m, kDamageDiffMaxEdits));
    const uint64_t* a = prev.data() + head;
    const uint64_t* b = cur.data() + head;
    static thread_local std::vector<Index> v;
    static thread_local std::vector<Index> trace;
    v.assign(static_cast<size_t>(2 * maxD + 3), 0);
    trace.clear();
    auto at = [&](Index k) -> Index& { return v[static_cast<size_t>(k + maxD + 1)]; };
    Index edits = -1;
    for (Index d = 0; d <= maxD && edits < 0; ++d) {
        for (Index k = -d; k <= d; k += 2) {
            Index x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
            Index y = x - k;
            while (x < n0 && y < m0 && a[x] == b[y]) {
                ++x;
                ++y;
            }
            at(k) = x;
            if (x >= n0 && y >= m0) {
                edits = d;
            }
        }
        for (Index k = -d; k <= d; ++k) {
            trace.push_back(at(k));
        }
    }
    if (edits < 0) {
        return false;  // Too many edits: keep the single range (head and tail only).
    }

    // Backtrack from (n0, m0): each step undoes one edit and the diagonal run (matches) after it.
    Index x = n0;
    Index y = m0;
    for (Index d = edits; d > 0; --d) {
        const Index k = x - y;
        const Index* row = trace.data() + (d - 1) * (d - 1);  // Row d - 1; k' at row[k' + d - 1].
        auto prevRow = [&](Index kk) { return row[kk + d - 1]; };
        const bool down = k == -d || (k != d && prevRow(k - 1) < prevRow(k + 1));
        const Index prevK = down ? k + 1 : k - 1;
        const Index prevX = prevRow(prevK);
        const Index runStart = down ? prevX : prevX + 1;  // x right after this step's edit
        while (x > runStart) {
            --x;
            --y;
            link(head + static_cast<size_t>(x), head + static_cast<size_t>(y));
        }
        x = prevX;
        y = prevX - prevK;
    }
    while (x > 0 && y > 0) {
        --x;
        --y;
        link(head + static_cast<size_t>(x), head + static_cast<size_t>(y));
    }
    return true;
}

void hashCommandList(const CommandList& list, std::vector<uint64_t>& out) {
    out.clear();
    out.reserve(list.commands.size());
    for (const Command& cmd : list.commands) {
        uint8_t kind = static_cast<uint8_t>(cmd.kind);
        uint64_t hash = hashBytes(kFnvOffsetBasis, &kind, 1);
        switch (cmd.kind) {
            case CmdKind::FillPath: {
                const FillCmd& c = list.fills[cmd.index];
                hash = hashFields(hash, c);
                hash = hashPath(hash, list, c.path);
                hash = hashPool(hash, list.stops, c.paint.stopOffset, c.paint.stopCount);
                break;
            }
            case CmdKind::StrokePath: {
                const StrokeCmd& c = list.strokes[cmd.index];
                hash = hashFields(hash, c);
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
                hash = hashFields(hash, c);
                hash = hashPool(hash, list.glyphs, c.glyphOffset, c.glyphCount);
                hash = hashPool(hash, list.glyphPos, c.glyphOffset, c.glyphCount);
                break;
            }
            case CmdKind::PushClipRect:
                hash = hashPod(hash, list.clipRects[cmd.index]);
                break;
            case CmdKind::PushClipPath: {
                const ClipPathCmd& c = list.clipPaths[cmd.index];
                hash = hashFields(hash, c);
                hash = hashPath(hash, list, c.path);
                break;
            }
            case CmdKind::PushLayer:
                hash = hashFields(hash, list.layers[cmd.index]);
                break;
            case CmdKind::PushMask:
                hash = hashFields(hash, list.masks[cmd.index]);
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
                EffectCmd payload = c;
                payload.effectOffset = 0;
                hash = hashPod(hash, payload);
                hash = hashPool(hash, list.effects, c.effectOffset, c.effectCount);
                break;
            }
            case CmdKind::DrawParticles: {
                const ParticlesCmd& c = list.particles[cmd.index];
                ParticlesCmd payload = c;
                payload.instanceOffset = 0;
                hash = hashPod(hash, payload);
                hash = hashPool(hash, list.particleInstances, c.instanceOffset, c.count);
                break;
            }
            case CmdKind::DrawImageMesh: {
                const ImageMeshCmd& c = list.imageMeshes[cmd.index];
                ImageMeshCmd payload = c;
                payload.vertexOffset = 0;
                payload.indexOffset = 0;
                hash = hashPod(hash, payload);
                hash = hashPool(hash, list.meshVertices, c.vertexOffset, c.vertexCount);
                hash = hashPool(hash, list.meshIndices, c.indexOffset, c.indexCount);
                break;
            }
            case CmdKind::DrawBoxShadow:
                // T-668: all 4-byte fields (no padding, no pool offsets), so every field is hashed;
                // T-680's unused extra terms are zero.
                static_assert(sizeof(BoxShadowCmd) == 41 * sizeof(float), "BoxShadowCmd has padding");
                hash = hashPod(hash, list.boxShadows[cmd.index]);
                break;
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

namespace {

/// T-664's estimate of glyph run `cmd` (see glyphRunInkBox): the origins from the font's ascent
/// above to its descent below, half the size left of the first origin to one and a half sizes right
/// of the last. False when the metrics give no finite, positive ascent + descent.
bool glyphRunMetricBox(const CommandList& list, const GlyphsCmd& cmd, TextShaper& shaper, Rect& out) {
    const FontMetrics m = shaper.metrics(cmd.font);
    const float ascent = m.ascent * cmd.size;
    const float descent = std::fabs(m.descent) * cmd.size;  // FontMetrics gives it either sign
    if (!std::isfinite(ascent) || !std::isfinite(descent) || ascent < 0.0f || !(ascent + descent > 0.0f)) {
        return false;
    }
    const Vec2* pos = list.glyphPos.data() + cmd.glyphOffset;
    float minX = pos[0].x, minY = pos[0].y, maxX = pos[0].x, maxY = pos[0].y;
    for (uint32_t i = 1; i < cmd.glyphCount; ++i) {
        minX = std::min(minX, pos[i].x);
        minY = std::min(minY, pos[i].y);
        maxX = std::max(maxX, pos[i].x);
        maxY = std::max(maxY, pos[i].y);
    }
    const float half = 0.5f * cmd.size;
    const float x0 = minX - half, x1 = maxX + cmd.size + half;
    out = Rect{x0, minY - ascent, x1 - x0, (maxY - minY) + ascent + descent};
    return true;
}

/// T-684 (Docs/tasks/T-684.md item 2): one glyph's outline control-point box in em units
/// (untransformed), or that the shaper gives no outline for it (`outline` false), keyed by the
/// shaper's generation (T-675: never reused by another shaper), the font and the glyph id.
struct GlyphOutlineBox {
    uint64_t generation = 0;
    FontId font = 0;
    uint32_t glyph = 0;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool outline = false;
};
constexpr size_t kMaxGlyphOutlineBoxes = 4096;

bool glyphOutlineBoxLess(const GlyphOutlineBox& a, const GlyphOutlineBox& b) {
    if (a.generation != b.generation) {
        return a.generation < b.generation;
    }
    if (a.font != b.font) {
        return a.font < b.font;
    }
    return a.glyph < b.glyph;
}

/// The control-point box of `glyph`'s outline (false: no outline, or fewer than one point), from the
/// per-thread cache; on a miss from TextShaper::glyphOutline(), remembered. The cache holds at most
/// kMaxGlyphOutlineBoxes entries and is emptied when full; its storage is reserved once, so neither
/// a hit nor a miss allocates once warm.
bool glyphOutlineBox(TextShaper& shaper, FontId font, uint32_t glyph, GlyphOutlineBox& out) {
    static thread_local std::vector<GlyphOutlineBox> boxes;
    static thread_local std::vector<PathVerb> verbs;
    static thread_local std::vector<float> points;
    GlyphOutlineBox probe;
    probe.generation = shaper.generation();
    probe.font = font;
    probe.glyph = glyph;
    auto it = std::lower_bound(boxes.begin(), boxes.end(), probe, glyphOutlineBoxLess);
    if (it != boxes.end() && it->generation == probe.generation && it->font == font && it->glyph == glyph) {
        out = *it;
        return out.outline;
    }
    verbs.clear();
    points.clear();
    if (shaper.glyphOutline(font, glyph, verbs, points) && points.size() >= 2) {
        // The control points bound the outline (every segment lies in their convex hull).
        probe.outline = true;
        probe.x0 = probe.x1 = points[0];
        probe.y0 = probe.y1 = points[1];
        for (size_t k = 2; k + 1 < points.size(); k += 2) {
            probe.x0 = std::min(probe.x0, points[k]);
            probe.x1 = std::max(probe.x1, points[k]);
            probe.y0 = std::min(probe.y0, points[k + 1]);
            probe.y1 = std::max(probe.y1, points[k + 1]);
        }
    }
    if (boxes.capacity() < kMaxGlyphOutlineBoxes) {
        boxes.reserve(kMaxGlyphOutlineBoxes);
    }
    if (boxes.size() >= kMaxGlyphOutlineBoxes) {
        boxes.clear();
    }
    it = std::lower_bound(boxes.begin(), boxes.end(), probe, glyphOutlineBoxLess);
    boxes.insert(it, probe);
    out = probe;
    return out.outline;
}

/// glyphRunInkBounds() from run `cmd`'s glyphRunInkBox() result `local`.
bool inkBoundsOfBox(const GlyphsCmd& cmd, const Rect& local, Rect& out) {
    if (!(local.w > 0.0f) || !(local.h > 0.0f)) {
        out = Rect{};  // no ink
        return true;
    }
    out = transformedRect(cmd.xf, local);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.w) && std::isfinite(out.h);
}

}  // namespace

bool glyphRunInkBox(const CommandList& list, const GlyphsCmd& cmd, TextShaper* shaper, Rect& out, float dpr) {
    // T-664 (Docs/tasks/T-664.md item 5), moved from xt::gpu's clip_bounds.cpp by T-680; per glyph
    // since T-681 (Docs/tasks/T-681.md item 4).
    if (shaper == nullptr || cmd.glyphCount == 0 || cmd.glyphOffset > list.glyphPos.size() ||
        cmd.glyphCount > list.glyphPos.size() - cmd.glyphOffset || cmd.glyphOffset > list.glyphs.size() ||
        cmd.glyphCount > list.glyphs.size() - cmd.glyphOffset || !std::isfinite(cmd.size) ||
        !std::isfinite(cmd.effectRadius) || !std::isfinite(dpr) || !(dpr > 0.0f)) {
        return false;
    }
    // One device pixel in run units: 1 / (dpr x the smallest singular value of cmd.xf's linear part).
    const double a = cmd.xf.a, b = cmd.xf.b, c = cmd.xf.c, d = cmd.xf.d;
    const double sum = a * a + b * b + c * c + d * d;
    const double det = a * d - b * c;
    const double root = std::sqrt(std::max(0.0, sum * sum - 4.0 * det * det));
    const double minScale = std::sqrt(std::max(0.0, 0.5 * (sum - root)));
    if (!std::isfinite(minScale) || !(minScale > 1e-6)) {
        return false;  // a degenerate (or non-finite) transform
    }
    const double devicePx = 1.0 / (static_cast<double>(dpr) * minScale);
    // The bitmap size of a glyph without an outline: the run's size at its device scale, on the
    // 1/4 px grid the shaper's bitmap cache keys on.
    const double deviceScale = static_cast<double>(dpr) * std::sqrt(std::fabs(det));
    const float bitmapSize =
        static_cast<float>(std::floor(static_cast<double>(cmd.size) * deviceScale * 4.0 + 0.5) / 4.0);
    // A run drawn from lightly hinted bitmaps (xt::sw below 24 device px, the GPU glyph atlas on a
    // single-sampled target: glyphRunUsesHintedBitmap) can reach past its outline -- the hinting
    // moves edges to the pixel grid and the baseline is rounded to a whole pixel -- so each glyph
    // also takes its bitmap's box there.
    const bool hinted = glyphRunUsesHintedBitmap(cmd, mul(scaling(Vec2{dpr, dpr}), cmd.xf));
    // The scratch keeps its capacity across calls (no allocation per frame once warm).
    static thread_local GlyphBitmap bitmap;
    const Vec2* pos = list.glyphPos.data() + cmd.glyphOffset;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    bool ink = false;
    bool measured = false;  // the shaper answered for some glyph (an outline or a bitmap)
    auto add = [&](double gx0, double gy0, double gx1, double gy1) {
        if (!ink) {
            x0 = gx0, y0 = gy0, x1 = gx1, y1 = gy1;
            ink = true;
            return;
        }
        x0 = std::min(x0, gx0), y0 = std::min(y0, gy0);
        x1 = std::max(x1, gx1), y1 = std::max(y1, gy1);
    };
    for (uint32_t i = 0; i < cmd.glyphCount; ++i) {
        const uint32_t glyphId = list.glyphs[cmd.glyphOffset + i];
        GlyphOutlineBox e;
        if (glyphOutlineBox(*shaper, cmd.font, glyphId, e)) {
            // The outline's control-point box (T-684: cached per glyph).
            measured = true;
            const double s = cmd.size;
            add(pos[i].x + e.x0 * s, pos[i].y + e.y0 * s, pos[i].x + e.x1 * s, pos[i].y + e.y1 * s);
            if (hinted && bitmapSize > 0.0f &&
                shaper->glyphBitmap(cmd.font, glyphId, bitmapSize, 0.0f, bitmap) && bitmap.width > 0 &&
                bitmap.height > 0) {
                // The phase-0 bitmap from the floor of the origin: one device pixel more on either
                // side for the floor and the 1/4 px phases, half a pixel above and below for the
                // rounded baseline.
                const double k = 1.0 / deviceScale;
                add(pos[i].x + (bitmap.bearingX - 1.0) * k, pos[i].y - (bitmap.bearingY + 0.5) * k,
                    pos[i].x + (bitmap.bearingX + bitmap.width + 1.0) * k,
                    pos[i].y + (bitmap.height - bitmap.bearingY + 0.5) * k);
            }
            continue;
        }
        // No outline (a space; a bitmap-only glyph such as an emoji): the bitmap's own box, in
        // device px from the floor of the origin, brought back to run units, grown by one device
        // pixel more for the pixel grid and the subpixel phase.
        if (bitmapSize > 0.0f && shaper->glyphBitmap(cmd.font, glyphId, bitmapSize, 0.0f, bitmap)) {
            measured = true;
            if (bitmap.width > 0 && bitmap.height > 0) {
                const double k = 1.0 / deviceScale;
                add(pos[i].x + (bitmap.bearingX - 1.0) * k, pos[i].y - (bitmap.bearingY + 1.0) * k,
                    pos[i].x + (bitmap.bearingX + bitmap.width + 1.0) * k,
                    pos[i].y + (bitmap.height - bitmap.bearingY + 1.0) * k);
            }
        }
        // Neither: no backend draws this glyph (xt::sw skips it, the GPU atlas falls back to the
        // missing outline), so it adds no ink.
    }
    const double effectGrow = 2.0 * static_cast<double>(cmd.effectRadius);  // T-178's effect reach
    if (!measured) {
        // The shaper gives no outlines or bitmaps at all: T-664's estimate.
        Rect estimate;
        if (!glyphRunMetricBox(list, cmd, *shaper, estimate)) {
            return false;
        }
        const float g = static_cast<float>(effectGrow);
        out = Rect{estimate.x - g, estimate.y - g, estimate.w + 2.0f * g, estimate.h + 2.0f * g};
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.w) && std::isfinite(out.h);
    }
    if (!ink) {
        out = Rect{};  // spaces only: nothing is drawn
        return true;
    }
    const double grow = devicePx + effectGrow;
    out = Rect{static_cast<float>(x0 - grow), static_cast<float>(y0 - grow),
               static_cast<float>(x1 - x0 + 2.0 * grow), static_cast<float>(y1 - y0 + 2.0 * grow)};
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.w) && std::isfinite(out.h);
}

bool glyphRunInkBounds(const CommandList& list, const GlyphsCmd& cmd, TextShaper* shaper, Rect& out,
                       float dpr) {
    Rect local;
    if (!glyphRunInkBox(list, cmd, shaper, local, dpr)) {
        return false;
    }
    return inkBoundsOfBox(cmd, local, out);
}

bool GlyphInkMemo::inkBox(const CommandList& list, uint32_t runIndex, TextShaper* shaper, Rect& out,
                          float dpr) {
    // T-684 (Docs/tasks/T-684.md item 2).
    const GlyphsCmd& cmd = list.glyphRuns[runIndex];
    if (shaper == nullptr) {
        return glyphRunInkBox(list, cmd, shaper, out, dpr);  // asks no shaper: nothing to remember
    }
    if (runIndex >= m_entries.size()) {
        m_entries.resize(std::max<size_t>(list.glyphRuns.size(), size_t{runIndex} + 1));
    }
    Entry& e = m_entries[runIndex];
    const uint64_t generation = shaper->generation();
    // Bitwise comparison of the floats: -0 / +0 and NaN payloads recompute (never a wrong hit).
    const bool hit = e.epoch == m_epoch && e.generation == generation &&
                     std::memcmp(&e.xf, &cmd.xf, sizeof(Mat2x3)) == 0 && e.font == cmd.font &&
                     std::memcmp(&e.size, &cmd.size, sizeof(float)) == 0 &&
                     std::memcmp(&e.effectRadius, &cmd.effectRadius, sizeof(float)) == 0 &&
                     std::memcmp(&e.dpr, &dpr, sizeof(float)) == 0 && e.glyphOffset == cmd.glyphOffset &&
                     e.glyphCount == cmd.glyphCount && e.rendering == cmd.rendering;
    if (!hit) {
        e.epoch = m_epoch;
        e.generation = generation;
        e.xf = cmd.xf;
        e.font = cmd.font;
        e.size = cmd.size;
        e.effectRadius = cmd.effectRadius;
        e.dpr = dpr;
        e.glyphOffset = cmd.glyphOffset;
        e.glyphCount = cmd.glyphCount;
        e.rendering = cmd.rendering;
        e.box = Rect{};
        e.ok = glyphRunInkBox(list, cmd, shaper, e.box, dpr);
    }
    if (e.ok) {
        out = e.box;
    }
    return e.ok;
}

bool GlyphInkMemo::inkBounds(const CommandList& list, uint32_t runIndex, TextShaper* shaper, Rect& out,
                             float dpr) {
    // glyphRunInkBounds() over the remembered box.
    Rect local;
    if (!inkBox(list, runIndex, shaper, local, dpr)) {
        return false;
    }
    return inkBoundsOfBox(list.glyphRuns[runIndex], local, out);
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
            // B7 (Docs/tasks/T-263a.md): miter tips and square caps reach past width / 2; the
            // same reach the Painter's node bounds use.
            return outset(box, strokeOutset(c.width, c.cap, c.join, c.miterLimit) * maxScale(c.xf));
        }
        case CmdKind::DrawImage: {
            const ImageCmd& c = list.images[cmd.index];
            return transformedRect(c.xf, c.dst);
        }
        case CmdKind::DrawGlyphs: {
            const GlyphsCmd& c = list.glyphRuns[cmd.index];
            Rect box;
            if (!glyphRunBox(list, c, box)) {
                return Rect{};
            }
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
            // (effectExpand(), Docs/design/18-effects.md §18.3) and clipped to the viewport.
            return list.effectCmds[cmd.index].boundsHint;
        case CmdKind::PushProjection:
            // ProjectionCmd::boundsHint is the projected subtree's screen AABB, already outset by
            // the subtree's largest effect expansion and clipped to the viewport (T-105,
            // Docs/design/19-3d-space.md §19.6 step 1); it is the whole viewport when the Painter
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
        case CmdKind::DrawBoxShadow: {
            // T-668: R' outset by 3 sigma (xt/box_shadow.h), the quad a backend draws.
            const BoxShadowCmd& c = list.boxShadows[cmd.index];
            return transformedRect(c.xf, boxShadowDrawRect(c));
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

bool commandBoundsInFrame(const CommandList& list, const Command& cmd, const Mat2x3& frame, Rect& out,
                          bool& empty, TextShaper* shaper, GlyphInkMemo* memo) {
    // T-668 (Docs/tasks/T-668.md item 2): commandBounds()'s boxes, but each draw command's own
    // geometry box is mapped through `frame * xf` directly instead of through the root-box AABB, so
    // a rotated node's children are not inflated by a second axis-aligned box.
    empty = false;
    out = Rect{};
    switch (cmd.kind) {
        case CmdKind::FillPath: {
            const FillCmd& c = list.fills[cmd.index];
            if (c.path.pointCount < 2 || c.path.pointOffset + c.path.pointCount > list.points.size()) {
                empty = true;
                return true;
            }
            out = transformedPath(list, mul(frame, c.xf), c.path);
            break;
        }
        case CmdKind::StrokePath: {
            const StrokeCmd& c = list.strokes[cmd.index];
            if (c.path.pointCount < 2 || c.path.pointOffset + c.path.pointCount > list.points.size()) {
                empty = true;
                return true;
            }
            const Mat2x3 m = mul(frame, c.xf);
            // Always grown (commandBounds leaves a zero-size box alone; a zero-length stroke still
            // draws its caps).
            out = outset(transformedPath(list, m, c.path),
                         strokeOutset(c.width, c.cap, c.join, c.miterLimit) * maxScale(m));
            break;
        }
        case CmdKind::DrawImage: {
            const ImageCmd& c = list.images[cmd.index];
            out = transformedRect(mul(frame, c.xf), c.dst);
            break;
        }
        case CmdKind::DrawGlyphs: {
            const GlyphsCmd& c = list.glyphRuns[cmd.index];
            Rect box;
            if (!glyphRunBox(list, c, box)) {
                empty = true;
                return true;
            }
            // T-680 ("補充 2" B.2): the font-metric box where the shaper gives one; T-681: the
            // glyphs' own ink boxes (dpr 1: the Painter paints for any ratio), none for spaces.
            // T-684: remembered per build / paint with a memo.
            Rect ink;
            if (memo != nullptr ? memo->inkBox(list, cmd.index, shaper, ink)
                                : glyphRunInkBox(list, c, shaper, ink)) {
                if (!(ink.w > 0.0f) || !(ink.h > 0.0f)) {
                    empty = true;
                    return true;
                }
                box = ink;
            }
            out = transformedRect(mul(frame, c.xf), box);
            break;
        }
        case CmdKind::PushClipRect: {
            const ClipRectCmd& c = list.clipRects[cmd.index];
            out = transformedRect(mul(frame, c.xf), c.rect);
            break;
        }
        case CmdKind::PushClipPath: {
            const ClipPathCmd& c = list.clipPaths[cmd.index];
            if (c.path.pointCount < 2 || c.path.pointOffset + c.path.pointCount > list.points.size()) {
                empty = true;
                return true;
            }
            out = transformedPath(list, mul(frame, c.xf), c.path);
            break;
        }
        case CmdKind::DrawBoxShadow: {
            const BoxShadowCmd& c = list.boxShadows[cmd.index];
            out = transformedRect(mul(frame, c.xf), boxShadowDrawRect(c));
            break;
        }
        case CmdKind::PushLayer:
        case CmdKind::PushMask:
        case CmdKind::PushEffect:
        case CmdKind::PushProjection:
        case CmdKind::DrawImageQuad:
        case CmdKind::DrawParticles:
        case CmdKind::DrawImageMesh: {
            // Root-box boxes (a group's window, a quad's / mesh's points, particles).
            const Rect r = commandBounds(list, cmd);
            if (cmd.kind == CmdKind::DrawParticles || cmd.kind == CmdKind::DrawImageMesh) {
                if (!(r.w > 0.0f) && !(r.h > 0.0f) && r.x == 0.0f && r.y == 0.0f) {
                    empty = true;  // no instances / no vertices
                    return true;
                }
            }
            out = transformedRect(frame, r);
            break;
        }
        case CmdKind::PushTransform3D:
        case CmdKind::PopClip:
        case CmdKind::PopLayer:
        case CmdKind::EndMaskContent:
        case CmdKind::PopMask:
        case CmdKind::PopProjection:
        case CmdKind::PopTransform3D:
        case CmdKind::PopEffect:
            empty = true;
            return true;
    }
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.w) && std::isfinite(out.h);
}

void computeDamage(const CommandList& prev, const CommandList& cur, Rect viewportDevice, DamageList& out) {
    out.clear();
    out.viewport = viewportDevice;

    // §23.2 step 2 (T-644 revision): hash both lists and align them; the unmatched commands form
    // any number of changed ranges, and the matched ones between them stay clean.
    static thread_local std::vector<uint64_t> prevHashes;
    static thread_local std::vector<uint64_t> curHashes;
    static thread_local std::vector<uint32_t> prevToCur;
    static thread_local std::vector<uint32_t> curToPrev;
    static thread_local std::vector<uint8_t> prevDirty;
    static thread_local std::vector<uint8_t> curDirty;
    /// Indices of the PushEffect commands open at the current command (B6 below).
    static thread_local std::vector<size_t> openEffects;
    hashCommandList(prev, prevHashes);
    hashCommandList(cur, curHashes);

    // T-203 (Docs/design/18-effects.md §18.7): a custom effect's shader may read `xtTime`, so
    // its node changes on screen whenever the clock moves even though no command did.
    const bool clockMoved = !cur.shaders.empty() && cur.time != prev.time;
    if (prevHashes == curHashes && !clockMoved) {
        return;  // Identical lists: nothing changed on screen.
    }
    (void)matchCommandHashes(prevHashes, curHashes, prevToCur, curToPrev);
    markDirty(prev, cur, prevToCur, curToPrev, prevDirty, curDirty);

    // §23.2 step 3: the changed ranges' bounds, in device pixels, clipped to the viewport.
    float dpr = cur.devicePixelRatio > 0 ? cur.devicePixelRatio : 1.0f;
    /// T-332: the bounding boxes of the PushClipPath commands open at the current command, each
    /// already intersected with the ones outside it (a PushClipRect pushes its enclosing entry
    /// unchanged, so overflow clips keep their pre-T-332 behaviour).
    static thread_local std::vector<Rect> openClips;
    auto collect = [&](const CommandList& list, const std::vector<uint8_t>& dirty, float listDpr) {
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
        openEffects.clear();
        bool inRange = false;
        for (size_t i = 0; i < list.commands.size(); ++i) {
            const Command& cmd = list.commands[i];
            if (dirty[i] != 0) {
                if (!inRange) {
                    // B6 (Docs/tasks/T-263a.md): an effect whose PushEffect is still open where a
                    // changed range starts encloses it, and its halo (a glow, a shadow, a blur)
                    // follows the content, so its boundsHint -- already outset by the stack's
                    // expansion -- is repainted too. Effects opened inside the range are covered by
                    // their own PushEffect.
                    for (size_t e : openEffects) {
                        add(commandBounds(list, list.commands[e]));
                    }
                }
                const Rect bounds = clipped(commandBounds(list, cmd));
                add(bounds);
                trackClip(cmd, bounds);
            } else if (cmd.kind == CmdKind::PushClipPath || cmd.kind == CmdKind::PushClipRect ||
                       cmd.kind == CmdKind::PopClip) {
                trackClip(cmd, clipped(commandBounds(list, cmd)));
            }
            inRange = dirty[i] != 0;
            if (cmd.kind == CmdKind::PushEffect) {
                openEffects.push_back(i);
            } else if (cmd.kind == CmdKind::PopEffect && !openEffects.empty()) {
                openEffects.pop_back();
            }
        }
    };
    float prevDpr = prev.devicePixelRatio > 0 ? prev.devicePixelRatio : 1.0f;
    collect(prev, prevDirty, prevDpr);
    collect(cur, curDirty, dpr);
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

    // Docs/design/18-effects.md §18.6 (T-130): a BackdropBlur node reads what was drawn below it,
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

    // Docs/design/18-effects.md §18.12 (T-326): a Layer composited with a non-Normal blend mode
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

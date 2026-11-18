#include <cmath>
#include <cstring>
#include <vector>

#include "damage_internal.h"

/// Per-command hashes of a CommandList (Docs/design/23-host-integration.md §23.2, Docs/tasks/T-98.md;
/// moved out of damage.cpp by T-721, unchanged) and their position-independent form
/// (Docs/design/10-backends.md §10.16 W2, Docs/tasks/T-721.md): hashCommandList() for the damage diff
/// and the absolute effect cache key, hashCommandRelative() for the key that survives a translation.

namespace xt {

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

// -------------------------------------------------------------------------------------------
// T-721: positions relative to an origin (CommandHashOrigin). `rel == nullptr` is the plain hash:
// every helper below then returns its argument untouched, so hashCommandList() hashes the same
// bytes as before.
// -------------------------------------------------------------------------------------------

/// `v - origin`, then (steps > 0) the nearest multiple of 1 / steps as a whole number. A zero origin
/// and zero steps leave `v` bit for bit (no arithmetic touches it).
float relativePos(float v, float origin, float steps) {
    if (origin != 0.0f) {
        v -= origin;
    }
    if (steps > 0.0f) {
        v = std::nearbyint(v * steps) + 0.0f;  // + 0: -0 and +0 are one value
    }
    return v;
}

float relativeLen(float v, float steps) { return steps > 0.0f ? std::nearbyint(v * steps) + 0.0f : v; }

Vec2 relativePoint(Vec2 p, Vec2 origin, float steps) {
    return Vec2{relativePos(p.x, origin.x, steps), relativePos(p.y, origin.y, steps)};
}

/// A command drawn in its plane (FillPath, StrokePath, DrawImage, DrawGlyphs, the clips,
/// DrawParticles, DrawBoxShadow): its transform's translation relative to the plane origin.
template <class Cmd>
const Cmd& planeRelative(const Cmd& cmd, Cmd& tmp, const CommandHashOrigin* rel) {
    if (rel == nullptr) {
        return cmd;
    }
    tmp = cmd;
    tmp.xf.e = relativePos(cmd.xf.e, rel->plane.x, rel->planeSteps);
    tmp.xf.f = relativePos(cmd.xf.f, rel->plane.y, rel->planeSteps);
    return tmp;
}

/// A screen-space box (boundsHint, contentBounds) relative to the screen origin. An empty one is the
/// lazy "whole viewport" default and stays as it is.
Rect screenRelative(const Rect& r, const CommandHashOrigin& rel) {
    if (!(r.w > 0.0f) || !(r.h > 0.0f)) {
        return r;
    }
    return Rect{relativePos(r.x, rel.screen.x, rel.screenSteps),
                relativePos(r.y, rel.screen.y, rel.screenSteps), relativeLen(r.w, rel.screenSteps),
                relativeLen(r.h, rel.screenSteps)};
}

/// The hash of one command (its kind, its payload, the pool ranges the payload refers to).
uint64_t hashOneCommand(const CommandList& list, const Command& cmd, const CommandHashOrigin* rel) {
    uint8_t kind = static_cast<uint8_t>(cmd.kind);
    uint64_t hash = hashBytes(kFnvOffsetBasis, &kind, 1);
    switch (cmd.kind) {
        case CmdKind::FillPath: {
            FillCmd tmp;
            const FillCmd& c = planeRelative(list.fills[cmd.index], tmp, rel);
            hash = hashFields(hash, c);
            hash = hashPath(hash, list, c.path);
            hash = hashPool(hash, list.stops, c.paint.stopOffset, c.paint.stopCount);
            break;
        }
        case CmdKind::StrokePath: {
            StrokeCmd tmp;
            const StrokeCmd& c = planeRelative(list.strokes[cmd.index], tmp, rel);
            hash = hashFields(hash, c);
            hash = hashPath(hash, list, c.path);
            hash = hashPool(hash, list.stops, c.paint.stopOffset, c.paint.stopCount);
            hash = hashPool(hash, list.dashes, c.dashOffset, c.dashCount);
            break;
        }
        case CmdKind::DrawImage: {
            ImageCmd tmp;
            hash = hashPod(hash, planeRelative(list.images[cmd.index], tmp, rel));
            break;
        }
        case CmdKind::DrawGlyphs: {
            GlyphsCmd tmp;
            const GlyphsCmd& c = planeRelative(list.glyphRuns[cmd.index], tmp, rel);
            hash = hashFields(hash, c);
            hash = hashPool(hash, list.glyphs, c.glyphOffset, c.glyphCount);
            hash = hashPool(hash, list.glyphPos, c.glyphOffset, c.glyphCount);
            break;
        }
        case CmdKind::PushClipRect: {
            ClipRectCmd tmp;
            hash = hashPod(hash, planeRelative(list.clipRects[cmd.index], tmp, rel));
            break;
        }
        case CmdKind::PushClipPath: {
            ClipPathCmd tmp;
            const ClipPathCmd& c = planeRelative(list.clipPaths[cmd.index], tmp, rel);
            hash = hashFields(hash, c);
            hash = hashPath(hash, list, c.path);
            break;
        }
        case CmdKind::PushLayer: {
            if (rel == nullptr) {
                hash = hashFields(hash, list.layers[cmd.index]);
                break;
            }
            LayerCmd c = list.layers[cmd.index];
            c.boundsHint = screenRelative(c.boundsHint, *rel);
            hash = hashFields(hash, c);
            break;
        }
        case CmdKind::PushMask: {
            if (rel == nullptr) {
                hash = hashFields(hash, list.masks[cmd.index]);
                break;
            }
            MaskCmd c = list.masks[cmd.index];
            c.boundsHint = screenRelative(c.boundsHint, *rel);
            hash = hashFields(hash, c);
            break;
        }
        case CmdKind::PushProjection: {
            if (rel == nullptr) {
                hash = hashPod(hash, list.projections[cmd.index]);
                break;
            }
            // The root's box sits in the outer plane, its bounds on the screen.
            ProjectionCmd c = list.projections[cmd.index];
            c.rootXf.e = relativePos(c.rootXf.e, rel->plane.x, rel->planeSteps);
            c.rootXf.f = relativePos(c.rootXf.f, rel->plane.y, rel->planeSteps);
            c.boundsHint = screenRelative(c.boundsHint, *rel);
            hash = hashPod(hash, c);
            break;
        }
        case CmdKind::PushTransform3D:
            hash = hashPod(hash, list.transforms3[cmd.index]);  // relative to its 3D space root
            break;
        case CmdKind::DrawImageQuad: {
            if (rel == nullptr) {
                hash = hashPod(hash, list.imageQuads[cmd.index]);
                break;
            }
            ImageQuadCmd c = list.imageQuads[cmd.index];
            for (Vec2& p : c.p) {
                p = relativePoint(p, rel->plane, rel->planeSteps);
            }
            hash = hashPod(hash, c);
            break;
        }
        case CmdKind::PushEffect: {
            const EffectCmd& c = list.effectCmds[cmd.index];
            EffectCmd payload = c;
            payload.effectOffset = 0;
            if (rel != nullptr) {
                // `xf` is plane -> screen: its translation is a screen position. Inside a 3D space
                // its linear part is a finite difference of projected points (Painter: jacobianAt),
                // whose last bits change as the node moves: `linearSteps` rounds that away.
                payload.xf.a = relativeLen(c.xf.a, rel->linearSteps);
                payload.xf.b = relativeLen(c.xf.b, rel->linearSteps);
                payload.xf.c = relativeLen(c.xf.c, rel->linearSteps);
                payload.xf.d = relativeLen(c.xf.d, rel->linearSteps);
                payload.xf.e = relativePos(c.xf.e, rel->screen.x, rel->screenSteps);
                payload.xf.f = relativePos(c.xf.f, rel->screen.y, rel->screenSteps);
                payload.boundsHint = screenRelative(c.boundsHint, *rel);
                payload.contentBounds = screenRelative(c.contentBounds, *rel);
            }
            hash = hashPod(hash, payload);
            hash = hashPool(hash, list.effects, c.effectOffset, c.effectCount);
            break;
        }
        case CmdKind::DrawParticles: {
            ParticlesCmd tmp;
            const ParticlesCmd& c = planeRelative(list.particles[cmd.index], tmp, rel);
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
            if (rel == nullptr) {
                hash = hashPool(hash, list.meshVertices, c.vertexOffset, c.vertexCount);
            } else if (c.vertexCount != 0 && c.vertexOffset <= list.meshVertices.size() &&
                       list.meshVertices.size() - c.vertexOffset >= c.vertexCount) {
                // The same bytes as the pool range, each position made relative first.
                for (uint32_t i = 0; i < c.vertexCount; ++i) {
                    MeshVertex v = list.meshVertices[c.vertexOffset + i];
                    v.p = relativePoint(v.p, rel->plane, rel->planeSteps);
                    hash = hashPod(hash, v);
                }
            }
            hash = hashPool(hash, list.meshIndices, c.indexOffset, c.indexCount);
            break;
        }
        case CmdKind::DrawBoxShadow: {
            // T-668: all 4-byte fields (no padding, no pool offsets), so every field is hashed;
            // T-680's unused extra terms are zero.
            static_assert(sizeof(BoxShadowCmd) == 41 * sizeof(float), "BoxShadowCmd has padding");
            BoxShadowCmd tmp;
            hash = hashPod(hash, planeRelative(list.boxShadows[cmd.index], tmp, rel));
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
    return hash;
}

}  // namespace

void hashCommandList(const CommandList& list, std::vector<uint64_t>& out) {
    out.clear();
    out.reserve(list.commands.size());
    for (const Command& cmd : list.commands) {
        out.push_back(hashOneCommand(list, cmd, nullptr));
    }
}

uint64_t hashCommandRelative(const CommandList& list, uint32_t index, const CommandHashOrigin& origin) {
    if (index >= list.commands.size()) {
        return kFnvOffsetBasis;
    }
    return hashOneCommand(list, list.commands[index], &origin);
}

}  // namespace xt

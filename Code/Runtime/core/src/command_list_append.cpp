#include "command_list_append.h"

#include <vector>

#include "xt/assert.h"

/// T-748: see command_list_append.h.

namespace xt {

namespace {

template <typename T>
uint32_t appendPool(std::vector<T>& dst, const std::vector<T>& src, uint32_t offset, uint32_t count) {
    const auto at = static_cast<uint32_t>(dst.size());
    if (count > 0) {
        dst.insert(dst.end(), src.begin() + offset, src.begin() + offset + count);
    }
    return at;
}

PathRef appendPath(const CommandList& src, const PathRef& path, CommandList& out) {
    PathRef ref = path;
    ref.verbOffset = appendPool(out.verbs, src.verbs, path.verbOffset, path.verbCount);
    ref.pointOffset = appendPool(out.points, src.points, path.pointOffset, path.pointCount);
    return ref;
}

PaintRef appendPaint(const CommandList& src, const PaintRef& paint, CommandList& out) {
    PaintRef ref = paint;
    if (paint.stopCount > 0) {
        ref.stopOffset = appendPool(out.stops, src.stops, paint.stopOffset, paint.stopCount);
    }
    return ref;
}

/// A Custom effect finds its ShaderRef by (`source`, asset): copy that entry once.
void appendShader(const CommandList& src, uint16_t source, AssetId asset, CommandList& out) {
    for (const ShaderRef& s : out.shaders) {
        if (s.source == source && s.asset == asset) {
            return;
        }
    }
    for (const ShaderRef& s : src.shaders) {
        if (s.source != source || s.asset != asset) {
            continue;
        }
        ShaderRef ref = s;
        ref.sourceOffset = appendPool(out.shaderText, src.shaderText, s.sourceOffset, s.sourceLength);
        ref.glslOffset = appendPool(out.shaderText, src.shaderText, s.glslOffset, s.glslLength);
        ref.namesOffset = appendPool(out.shaderText, src.shaderText, s.namesOffset, s.namesLength);
        ref.paramOffset = appendPool(out.shaderParams, src.shaderParams, s.paramOffset, s.paramCount);
        out.shaders.push_back(ref);
        return;
    }
}

template <typename T>
uint32_t appendPayload(std::vector<T>& dst, const T& value) {
    dst.push_back(value);
    return static_cast<uint32_t>(dst.size() - 1);
}

}  // namespace

void appendCommandRange(const CommandList& src, uint32_t first, uint32_t end, CommandList& out) {
    XT_ASSERT(first <= end && end <= src.commands.size());
    out.commands.reserve(out.commands.size() + (end - first));
    for (uint32_t i = first; i < end; ++i) {
        Command cmd = src.commands[i];
        switch (cmd.kind) {
            case CmdKind::FillPath: {
                FillCmd c = src.fills[cmd.index];
                c.path = appendPath(src, c.path, out);
                c.paint = appendPaint(src, c.paint, out);
                cmd.index = appendPayload(out.fills, c);
                break;
            }
            case CmdKind::StrokePath: {
                StrokeCmd c = src.strokes[cmd.index];
                c.path = appendPath(src, c.path, out);
                c.paint = appendPaint(src, c.paint, out);
                c.dashOffset = appendPool(out.dashes, src.dashes, c.dashOffset, c.dashCount);
                cmd.index = appendPayload(out.strokes, c);
                break;
            }
            case CmdKind::DrawImage:
                cmd.index = appendPayload(out.images, src.images[cmd.index]);
                break;
            case CmdKind::DrawGlyphs: {
                GlyphsCmd c = src.glyphRuns[cmd.index];
                const uint32_t at = appendPool(out.glyphs, src.glyphs, c.glyphOffset, c.glyphCount);
                appendPool(out.glyphPos, src.glyphPos, c.glyphOffset, c.glyphCount);
                c.glyphOffset = at;
                cmd.index = appendPayload(out.glyphRuns, c);
                break;
            }
            case CmdKind::PushClipRect:
                cmd.index = appendPayload(out.clipRects, src.clipRects[cmd.index]);
                break;
            case CmdKind::PushClipPath: {
                ClipPathCmd c = src.clipPaths[cmd.index];
                c.path = appendPath(src, c.path, out);
                cmd.index = appendPayload(out.clipPaths, c);
                break;
            }
            case CmdKind::PushLayer:
                cmd.index = appendPayload(out.layers, src.layers[cmd.index]);
                break;
            case CmdKind::PushMask:
                cmd.index = appendPayload(out.masks, src.masks[cmd.index]);
                break;
            case CmdKind::PushProjection:
                cmd.index = appendPayload(out.projections, src.projections[cmd.index]);
                break;
            case CmdKind::PushTransform3D:
                cmd.index = appendPayload(out.transforms3, src.transforms3[cmd.index]);
                break;
            case CmdKind::DrawImageQuad:
                cmd.index = appendPayload(out.imageQuads, src.imageQuads[cmd.index]);
                break;
            case CmdKind::PushEffect: {
                EffectCmd c = src.effectCmds[cmd.index];
                const auto at = static_cast<uint32_t>(out.effects.size());
                for (uint32_t e = 0; e < c.effectCount; ++e) {
                    EffectInstance inst = src.effects[c.effectOffset + e];
                    if (inst.stopCount > 0) {
                        inst.stopOffset = appendPool(out.stops, src.stops, inst.stopOffset, inst.stopCount);
                    }
                    if (inst.kind == EffectKind::Custom) {
                        appendShader(src, c.source, inst.shader, out);
                    }
                    out.effects.push_back(inst);
                }
                c.effectOffset = at;
                cmd.index = appendPayload(out.effectCmds, c);
                break;
            }
            case CmdKind::DrawParticles: {
                ParticlesCmd c = src.particles[cmd.index];
                c.instanceOffset =
                    appendPool(out.particleInstances, src.particleInstances, c.instanceOffset, c.count);
                cmd.index = appendPayload(out.particles, c);
                break;
            }
            case CmdKind::DrawImageMesh: {
                ImageMeshCmd c = src.imageMeshes[cmd.index];
                c.vertexOffset =
                    appendPool(out.meshVertices, src.meshVertices, c.vertexOffset, c.vertexCount);
                c.indexOffset = appendPool(out.meshIndices, src.meshIndices, c.indexOffset, c.indexCount);
                cmd.index = appendPayload(out.imageMeshes, c);
                break;
            }
            case CmdKind::DrawBoxShadow:
                cmd.index = appendPayload(out.boxShadows, src.boxShadows[cmd.index]);
                break;
            case CmdKind::PopClip:
            case CmdKind::PopLayer:
            case CmdKind::EndMaskContent:
            case CmdKind::PopMask:
            case CmdKind::PopProjection:
            case CmdKind::PopTransform3D:
            case CmdKind::PopEffect:
                break;  // No payload.
        }
        out.commands.push_back(cmd);
    }
}

}  // namespace xt

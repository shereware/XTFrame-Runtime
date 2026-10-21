#include "xt/command_list.h"

#include <cstdio>

/// CommandList::clear() and the dumpCommandList debug serializer.
/// See docs/design/09-command-list.md §9.1, docs/tasks/T-10.md "command_list.h".

namespace xt {

void CommandList::clear() {
    commands.clear();
    fills.clear();
    strokes.clear();
    images.clear();
    glyphRuns.clear();
    clipRects.clear();
    clipPaths.clear();
    layers.clear();
    masks.clear();
    projections.clear();
    transforms3.clear();
    imageQuads.clear();
    effectCmds.clear();
    effects.clear();
    shaders.clear();
    shaderParams.clear();
    shaderText.clear();
    verbs.clear();
    points.clear();
    stops.clear();
    glyphs.clear();
    glyphPos.clear();
    dashes.clear();
    particles.clear();
    particleInstances.clear();
    imageMeshes.clear();
    meshVertices.clear();
    meshIndices.clear();
    structureChecked = false;
    frameSerial = 0;
    sources.clear();
    // viewport / devicePixelRatio intentionally left untouched.
}

namespace {

void appendFloat(std::string& out, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    out += buf;
}

const char* fillRuleName(FillRule rule) {
    switch (rule) {
        case FillRule::NonZero:
            return "NonZero";
        case FillRule::EvenOdd:
            return "EvenOdd";
    }
    return "?";
}

const char* lineCapName(LineCap cap) {
    switch (cap) {
        case LineCap::Butt:
            return "Butt";
        case LineCap::Round:
            return "Round";
        case LineCap::Square:
            return "Square";
    }
    return "?";
}

const char* lineJoinName(LineJoin join) {
    switch (join) {
        case LineJoin::Miter:
            return "Miter";
        case LineJoin::Round:
            return "Round";
        case LineJoin::Bevel:
            return "Bevel";
    }
    return "?";
}

const char* blendModeName(BlendMode blend) {
    switch (blend) {
        case BlendMode::Normal:
            return "Normal";
        case BlendMode::Multiply:
            return "Multiply";
        case BlendMode::Screen:
            return "Screen";
        case BlendMode::Overlay:
            return "Overlay";
        case BlendMode::Darken:
            return "Darken";
        case BlendMode::Lighten:
            return "Lighten";
        case BlendMode::ColorDodge:
            return "ColorDodge";
        case BlendMode::ColorBurn:
            return "ColorBurn";
        case BlendMode::HardLight:
            return "HardLight";
        case BlendMode::SoftLight:
            return "SoftLight";
        case BlendMode::Difference:
            return "Difference";
        case BlendMode::Exclusion:
            return "Exclusion";
        case BlendMode::Hue:
            return "Hue";
        case BlendMode::Saturation:
            return "Saturation";
        case BlendMode::Color:
            return "Color";
        case BlendMode::Luminosity:
            return "Luminosity";
        case BlendMode::Add:
            return "Add";
    }
    return "?";
}

const char* maskModeName(MaskMode mode) {
    switch (mode) {
        case MaskMode::Alpha:
            return "Alpha";
        case MaskMode::AlphaInverted:
            return "AlphaInverted";
        case MaskMode::Luma:
            return "Luma";
        case MaskMode::LumaInverted:
            return "LumaInverted";
        case MaskMode::Add:
            return "Add";
        case MaskMode::Subtract:
            return "Subtract";
        case MaskMode::Intersect:
            return "Intersect";
    }
    return "?";
}

const char* paintKindName(PaintKind kind) {
    switch (kind) {
        case PaintKind::None:
            return "None";
        case PaintKind::Solid:
            return "Solid";
        case PaintKind::Linear:
            return "Linear";
        case PaintKind::Radial:
            return "Radial";
        case PaintKind::Conic:
            return "Conic";
    }
    return "?";
}

void appendXf(std::string& out, const Mat2x3& xf) {
    out += "xf=[";
    appendFloat(out, xf.a);
    out += ' ';
    appendFloat(out, xf.b);
    out += ' ';
    appendFloat(out, xf.c);
    out += ' ';
    appendFloat(out, xf.d);
    out += ' ';
    appendFloat(out, xf.e);
    out += ' ';
    appendFloat(out, xf.f);
    out += ']';
}

void appendMat4(std::string& out, const Mat4& m) {
    out += '[';
    for (int i = 0; i < 16; ++i) {
        if (i != 0) {
            out += ' ';
        }
        appendFloat(out, m.m[i]);
    }
    out += ']';
}

void appendRect(std::string& out, const Rect& r) {
    out += '(';
    appendFloat(out, r.x);
    out += ',';
    appendFloat(out, r.y);
    out += ',';
    appendFloat(out, r.w);
    out += ',';
    appendFloat(out, r.h);
    out += ')';
}

const char* effectKindName(EffectKind kind) {
    switch (kind) {
        case EffectKind::None:
            return "None";
        case EffectKind::DropShadow:
            return "DropShadow";
        case EffectKind::Glow:
            return "Glow";
        case EffectKind::Blur:
            return "Blur";
        case EffectKind::ColorAdjust:
            return "ColorAdjust";
        case EffectKind::GradientMask:
            return "GradientMask";
        case EffectKind::Shine:
            return "Shine";
        case EffectKind::Outline:
            return "Outline";
        case EffectKind::BackdropBlur:
            return "BackdropBlur";
        case EffectKind::Custom:
            return "Custom";
    }
    return "?";
}

/// T-263d: ` source=<n>` after an asset command's line, only in a list that names its sources (so
/// the dump of a hand-built, baked or directly painted list is what it was).
void appendSource(std::string& out, const CommandList& list, uint16_t source) {
    if (!list.sources.empty()) {
        out += " source=";
        out += std::to_string(source);
    }
}

void appendPaint(std::string& out, const PaintRef& paint) {
    out += "paint=";
    out += paintKindName(paint.kind);
    if (paint.kind == PaintKind::Linear || paint.kind == PaintKind::Radial ||
        paint.kind == PaintKind::Conic) {
        out += '(';
        appendFloat(out, paint.p0.x);
        out += ',';
        appendFloat(out, paint.p0.y);
        out += "->";
        appendFloat(out, paint.p1.x);
        out += ',';
        appendFloat(out, paint.p1.y);
        out += ",stops=";
        out += std::to_string(paint.stopCount);
        out += ')';
    } else {
        out += '(';
        appendFloat(out, paint.color.r);
        out += ',';
        appendFloat(out, paint.color.g);
        out += ',';
        appendFloat(out, paint.color.b);
        out += ',';
        appendFloat(out, paint.color.a);
        out += ')';
    }
}

}  // namespace

void dumpCommandList(const CommandList& list, std::string& out) {
    out.clear();
    for (const Command& cmd : list.commands) {
        switch (cmd.kind) {
            case CmdKind::FillPath: {
                const FillCmd& fill = list.fills[cmd.index];
                std::string line;
                appendXf(line, fill.xf);
                line += " verbs=";
                line += std::to_string(fill.path.verbCount);
                line += " points=";
                line += std::to_string(fill.path.pointCount);
                line += " rule=";
                line += fillRuleName(fill.rule);
                line += ' ';
                appendPaint(line, fill.paint);
                out += "FillPath " + line;
                break;
            }
            case CmdKind::StrokePath: {
                const StrokeCmd& stroke = list.strokes[cmd.index];
                std::string line;
                appendXf(line, stroke.xf);
                line += " verbs=";
                line += std::to_string(stroke.path.verbCount);
                line += " points=";
                line += std::to_string(stroke.path.pointCount);
                line += ' ';
                appendPaint(line, stroke.paint);
                line += " width=";
                appendFloat(line, stroke.width);
                line += " cap=";
                line += lineCapName(stroke.cap);
                line += " join=";
                line += lineJoinName(stroke.join);
                line += " miter=";
                appendFloat(line, stroke.miterLimit);
                line += " dashes=";
                line += std::to_string(stroke.dashCount);
                out += "StrokePath " + line;
                break;
            }
            case CmdKind::DrawImage: {
                const ImageCmd& image = list.images[cmd.index];
                out += "DrawImage asset=";
                out += std::to_string(image.asset);
                out += " dst=(";
                appendFloat(out, image.dst.x);
                out += ',';
                appendFloat(out, image.dst.y);
                out += ',';
                appendFloat(out, image.dst.w);
                out += ',';
                appendFloat(out, image.dst.h);
                out += ") opacity=";
                appendFloat(out, image.opacity);
                appendSource(out, list, image.source);
                break;
            }
            case CmdKind::DrawGlyphs: {
                // Not produced by the T-10 Painter (Element/Text draw in T-27); reserved.
                out += "DrawGlyphs";
                break;
            }
            case CmdKind::PushClipRect: {
                const ClipRectCmd& clip = list.clipRects[cmd.index];
                out += "PushClipRect rect=(";
                appendFloat(out, clip.rect.x);
                out += ',';
                appendFloat(out, clip.rect.y);
                out += ',';
                appendFloat(out, clip.rect.w);
                out += ',';
                appendFloat(out, clip.rect.h);
                out += ')';
                break;
            }
            case CmdKind::PushClipPath: {
                const ClipPathCmd& clip = list.clipPaths[cmd.index];
                out += "PushClipPath verbs=";
                out += std::to_string(clip.path.verbCount);
                break;
            }
            case CmdKind::PopClip:
                out += "PopClip";
                break;
            case CmdKind::PushLayer: {
                const LayerCmd& layer = list.layers[cmd.index];
                out += "PushLayer opacity=";
                appendFloat(out, layer.opacity);
                out += " blend=";
                out += blendModeName(layer.blend);
                break;
            }
            case CmdKind::PopLayer:
                out += "PopLayer";
                break;
            case CmdKind::PushMask: {
                const MaskCmd& mask = list.masks[cmd.index];
                out += "PushMask mode=";
                out += maskModeName(mask.mode);
                break;
            }
            case CmdKind::EndMaskContent:
                out += "EndMaskContent";
                break;
            case CmdKind::PopMask:
                out += "PopMask";
                break;
            case CmdKind::PushProjection: {
                const ProjectionCmd& proj = list.projections[cmd.index];
                out += "PushProjection viewProj=";
                appendMat4(out, proj.viewProj);
                out += " rootXf=[";
                appendFloat(out, proj.rootXf.a);
                out += ' ';
                appendFloat(out, proj.rootXf.b);
                out += ' ';
                appendFloat(out, proj.rootXf.c);
                out += ' ';
                appendFloat(out, proj.rootXf.d);
                out += ' ';
                appendFloat(out, proj.rootXf.e);
                out += ' ';
                appendFloat(out, proj.rootXf.f);
                out += "] box=(";
                appendFloat(out, proj.boxRect.x);
                out += ',';
                appendFloat(out, proj.boxRect.y);
                out += ',';
                appendFloat(out, proj.boxRect.w);
                out += ',';
                appendFloat(out, proj.boxRect.h);
                out += ") depth=";
                out += std::to_string(proj.depthTest);
                break;
            }
            case CmdKind::PopProjection:
                out += "PopProjection";
                break;
            case CmdKind::PushTransform3D: {
                const Transform3DCmd& xf3 = list.transforms3[cmd.index];
                out += "PushTransform3D ";
                appendMat4(out, xf3.world3);
                break;
            }
            case CmdKind::PopTransform3D:
                out += "PopTransform3D";
                break;
            case CmdKind::DrawImageQuad: {
                const ImageQuadCmd& quad = list.imageQuads[cmd.index];
                out += "DrawImageQuad asset=";
                out += std::to_string(quad.asset);
                out += " p=";
                for (int i = 0; i < 4; ++i) {
                    out += '(';
                    appendFloat(out, quad.p[i].x);
                    out += ',';
                    appendFloat(out, quad.p[i].y);
                    out += ')';
                }
                out += " uv=(";
                appendFloat(out, quad.srcUv.x);
                out += ',';
                appendFloat(out, quad.srcUv.y);
                out += ',';
                appendFloat(out, quad.srcUv.w);
                out += ',';
                appendFloat(out, quad.srcUv.h);
                out += ") opacity=";
                appendFloat(out, quad.opacity);
                out += " smooth=";
                out += std::to_string(quad.smooth);
                appendSource(out, list, quad.source);
                break;
            }
            case CmdKind::PushEffect: {
                const EffectCmd& effect = list.effectCmds[cmd.index];
                out += "PushEffect count=";
                out += std::to_string(effect.effectCount);
                out += ' ';
                appendXf(out, effect.xf);
                out += " bounds=";
                appendRect(out, effect.boundsHint);
                out += " content=";
                appendRect(out, effect.contentBounds);
                appendSource(out, list, effect.source);
                for (uint32_t i = 0; i < effect.effectCount; ++i) {
                    const EffectInstance& e = list.effects[effect.effectOffset + i];
                    out += '\n';
                    out += "  effect kind=";
                    out += effectKindName(e.kind);
                    out += " enabled=";
                    out += std::to_string(e.enabled);
                    out += " params=[";
                    for (int p = 0; p < 8; ++p) {
                        if (p != 0) {
                            out += ' ';
                        }
                        appendFloat(out, e.params[p]);
                    }
                    out += "] color0=(";
                    appendFloat(out, e.colors[0].r);
                    out += ',';
                    appendFloat(out, e.colors[0].g);
                    out += ',';
                    appendFloat(out, e.colors[0].b);
                    out += ',';
                    appendFloat(out, e.colors[0].a);
                    out += ") color1=(";
                    appendFloat(out, e.colors[1].r);
                    out += ',';
                    appendFloat(out, e.colors[1].g);
                    out += ',';
                    appendFloat(out, e.colors[1].b);
                    out += ',';
                    appendFloat(out, e.colors[1].a);
                    out += ") stops=";
                    out += std::to_string(e.stopCount);
                }
                break;
            }
            case CmdKind::PopEffect:
                out += "PopEffect";
                break;
            case CmdKind::DrawParticles: {
                // T-202: the header line, then one line per instance.
                const ParticlesCmd& pc = list.particles[cmd.index];
                out += "DrawParticles texture=";
                out += pc.texture == kInvalidAsset ? std::string("none") : std::to_string(pc.texture);
                out += pc.blend == ParticleBlend::Additive ? " blend=additive" : " blend=normal";
                out += " count=";
                out += std::to_string(pc.count);
                out += ' ';
                appendXf(out, pc.xf);
                appendSource(out, list, pc.source);
                for (uint32_t i = 0; i < pc.count; ++i) {
                    const ParticleInstance& p = list.particleInstances[pc.instanceOffset + i];
                    out += "\n  particle c=(";
                    appendFloat(out, p.x);
                    out += ',';
                    appendFloat(out, p.y);
                    out += ") size=(";
                    appendFloat(out, p.w);
                    out += ',';
                    appendFloat(out, p.h);
                    out += ") rot=";
                    appendFloat(out, p.rot);
                    out += " rgba=(";
                    appendFloat(out, p.rgba.r);
                    out += ',';
                    appendFloat(out, p.rgba.g);
                    out += ',';
                    appendFloat(out, p.rgba.b);
                    out += ',';
                    appendFloat(out, p.rgba.a);
                    out += ") uv=(";
                    appendFloat(out, p.uv0.x);
                    out += ',';
                    appendFloat(out, p.uv0.y);
                    out += ',';
                    appendFloat(out, p.uv1.x);
                    out += ',';
                    appendFloat(out, p.uv1.y);
                    out += ')';
                }
                break;
            }
            case CmdKind::DrawImageMesh: {
                // T-245: the header line, then one line per vertex and one per triangle.
                const ImageMeshCmd& mc = list.imageMeshes[cmd.index];
                out += "DrawImageMesh asset=";
                out += std::to_string(mc.asset);
                out += " vertices=";
                out += std::to_string(mc.vertexCount);
                out += " triangles=";
                out += std::to_string(mc.indexCount / 3);
                out += " opacity=";
                appendFloat(out, mc.opacity);
                out += " smooth=";
                out += std::to_string(mc.smooth);
                appendSource(out, list, mc.source);
                for (uint32_t i = 0; i < mc.vertexCount; ++i) {
                    const MeshVertex& v = list.meshVertices[mc.vertexOffset + i];
                    out += "\n  vertex p=(";
                    appendFloat(out, v.p.x);
                    out += ',';
                    appendFloat(out, v.p.y);
                    out += ") uv=(";
                    appendFloat(out, v.uv.x);
                    out += ',';
                    appendFloat(out, v.uv.y);
                    out += ')';
                }
                for (uint32_t i = 0; i + 2 < mc.indexCount; i += 3) {
                    out += "\n  tri ";
                    out += std::to_string(list.meshIndices[mc.indexOffset + i]);
                    out += ' ';
                    out += std::to_string(list.meshIndices[mc.indexOffset + i + 1]);
                    out += ' ';
                    out += std::to_string(list.meshIndices[mc.indexOffset + i + 2]);
                }
                break;
            }
        }
        out += '\n';
    }
}

namespace {

/// `count` elements starting at `offset` fit in a pool of `size` (64-bit, so a uint32 sum cannot
/// wrap). An empty range is accepted at any offset: nothing is read through it, and the Painter
/// leaves a disabled effect's stale stopOffset next to a zero stopCount.
bool rangeOk(uint32_t offset, uint32_t count, size_t size) {
    return count == 0 || static_cast<uint64_t>(offset) + count <= static_cast<uint64_t>(size);
}

bool pathOk(const CommandList& list, const PathRef& path) {
    if (!rangeOk(path.verbOffset, path.verbCount, list.verbs.size()) ||
        !rangeOk(path.pointOffset, path.pointCount, list.points.size())) {
        return false;
    }
    // CommandList::points holds floats: Move/Line take one (x, y) pair, Cubic three.
    uint64_t needed = 0;
    for (uint32_t i = 0; i < path.verbCount; ++i) {
        switch (list.verbs[static_cast<size_t>(path.verbOffset) + i]) {
            case PathVerb::Move:
            case PathVerb::Line:
                needed += 2;
                break;
            case PathVerb::Cubic:
                needed += 6;
                break;
            case PathVerb::Close:
                break;
            default:
                return false;
        }
    }
    return needed <= path.pointCount;
}

bool paintOk(const CommandList& list, const PaintRef& paint) {
    return rangeOk(paint.stopOffset, paint.stopCount, list.stops.size());
}

bool effectOk(const CommandList& list, const EffectInstance& effect) {
    return rangeOk(effect.stopOffset, effect.stopCount, list.stops.size());
}

/// One open Layer / Mask / Effect section. They share one stack in both backends (SW's
/// LayerStack, the GPU frame stack), so they must nest with one another.
enum class Section : uint8_t { Layer, MaskShape, MaskContent, Effect };

/// Sections deeper than this are found by scanning back instead of being looked up in the
/// fixed array, so validation never allocates and stays linear for any realistic nesting.
constexpr size_t kSectionArrayDepth = 64;

/// The innermost section still open before command `end` of a list whose prefix [0, end) already
/// passed validation and leaves at least one section open.
Section innermostSection(const CommandList& list, size_t end) {
    size_t closed = 0;
    bool contentStarted = false;
    for (size_t j = end; j-- > 0;) {
        switch (list.commands[j].kind) {
            case CmdKind::PopLayer:
            case CmdKind::PopMask:
            case CmdKind::PopEffect:
                ++closed;
                break;
            case CmdKind::EndMaskContent:
                contentStarted = contentStarted || closed == 0;
                break;
            case CmdKind::PushLayer:
                if (closed == 0) {
                    return Section::Layer;
                }
                --closed;
                break;
            case CmdKind::PushMask:
                if (closed == 0) {
                    return contentStarted ? Section::MaskContent : Section::MaskShape;
                }
                --closed;
                break;
            case CmdKind::PushEffect:
                if (closed == 0) {
                    return Section::Effect;
                }
                --closed;
                break;
            default:
                break;
        }
    }
    return Section::Layer;  // Unreachable while a section is open.
}

}  // namespace

bool validateCommandList(const CommandList& list) {
    for (const EffectInstance& e : list.effects) {
        if (!effectOk(list, e)) {
            return false;
        }
    }
    // T-263d: a list that names its sources is drawn through them; every asset command's (and
    // ShaderRef's) `source` must index the table. One that names none ignores the field.
    const size_t sourceCount = list.sources.size();
    auto sourceOk = [sourceCount](uint16_t source) { return sourceCount == 0 || source < sourceCount; };
    for (const ShaderRef& s : list.shaders) {
        if (!sourceOk(s.source) || !rangeOk(s.sourceOffset, s.sourceLength, list.shaderText.size()) ||
            !rangeOk(s.glslOffset, s.glslLength, list.shaderText.size()) ||
            !rangeOk(s.namesOffset, s.namesLength, list.shaderText.size()) ||
            !rangeOk(s.paramOffset, s.paramCount, list.shaderParams.size()) || !effectOk(list, s.fallback)) {
            return false;
        }
    }

    Section sections[kSectionArrayDepth];
    size_t sectionDepth = 0;
    size_t clipDepth = 0;
    size_t projectionDepth = 0;
    // The innermost open section (only called while sectionDepth > 0).
    auto top = [&](size_t i) {
        return sectionDepth <= kSectionArrayDepth ? sections[sectionDepth - 1] : innermostSection(list, i);
    };
    auto open = [&](Section s) {
        if (sectionDepth < kSectionArrayDepth) {
            sections[sectionDepth] = s;
        }
        ++sectionDepth;
    };

    for (size_t i = 0; i < list.commands.size(); ++i) {
        const Command& cmd = list.commands[i];
        const uint32_t index = cmd.index;
        switch (cmd.kind) {
            case CmdKind::FillPath:
                if (index >= list.fills.size() || !pathOk(list, list.fills[index].path) ||
                    !paintOk(list, list.fills[index].paint)) {
                    return false;
                }
                break;
            case CmdKind::StrokePath: {
                if (index >= list.strokes.size()) {
                    return false;
                }
                const StrokeCmd& c = list.strokes[index];
                if (!pathOk(list, c.path) || !paintOk(list, c.paint) ||
                    !rangeOk(c.dashOffset, c.dashCount, list.dashes.size())) {
                    return false;
                }
                break;
            }
            case CmdKind::DrawImage:
                if (index >= list.images.size() || !sourceOk(list.images[index].source)) {
                    return false;
                }
                break;
            case CmdKind::DrawGlyphs: {
                if (index >= list.glyphRuns.size()) {
                    return false;
                }
                const GlyphsCmd& c = list.glyphRuns[index];
                if (!rangeOk(c.glyphOffset, c.glyphCount, list.glyphs.size()) ||
                    !rangeOk(c.glyphOffset, c.glyphCount, list.glyphPos.size())) {
                    return false;
                }
                break;
            }
            case CmdKind::PushClipRect:
                if (index >= list.clipRects.size()) {
                    return false;
                }
                ++clipDepth;
                break;
            case CmdKind::PushClipPath:
                if (index >= list.clipPaths.size() || !pathOk(list, list.clipPaths[index].path)) {
                    return false;
                }
                ++clipDepth;
                break;
            case CmdKind::PopClip:
                if (clipDepth == 0) {
                    return false;
                }
                --clipDepth;
                break;
            case CmdKind::PushLayer:
                if (index >= list.layers.size()) {
                    return false;
                }
                open(Section::Layer);
                break;
            case CmdKind::PopLayer:
                if (sectionDepth == 0 || top(i) != Section::Layer) {
                    return false;
                }
                --sectionDepth;
                break;
            case CmdKind::PushMask:
                if (index >= list.masks.size()) {
                    return false;
                }
                open(Section::MaskShape);
                break;
            case CmdKind::EndMaskContent:
                if (sectionDepth == 0 || top(i) != Section::MaskShape) {
                    return false;
                }
                if (sectionDepth <= kSectionArrayDepth) {
                    sections[sectionDepth - 1] = Section::MaskContent;
                }
                break;
            case CmdKind::PopMask:
                if (sectionDepth == 0 || top(i) != Section::MaskContent) {
                    return false;
                }
                --sectionDepth;
                break;
            case CmdKind::PushProjection:
                if (index >= list.projections.size()) {
                    return false;
                }
                ++projectionDepth;
                break;
            case CmdKind::PopProjection:
                if (projectionDepth == 0) {
                    return false;
                }
                --projectionDepth;
                break;
            case CmdKind::PushTransform3D:
                if (index >= list.transforms3.size() || projectionDepth == 0) {
                    return false;
                }
                break;
            case CmdKind::PopTransform3D:
                if (projectionDepth == 0) {
                    return false;
                }
                break;
            case CmdKind::DrawImageQuad:
                if (index >= list.imageQuads.size() || !sourceOk(list.imageQuads[index].source)) {
                    return false;
                }
                break;
            case CmdKind::PushEffect: {
                if (index >= list.effectCmds.size()) {
                    return false;
                }
                const EffectCmd& c = list.effectCmds[index];
                if (!rangeOk(c.effectOffset, c.effectCount, list.effects.size()) || !sourceOk(c.source)) {
                    return false;
                }
                open(Section::Effect);
                break;
            }
            case CmdKind::PopEffect:
                if (sectionDepth == 0 || top(i) != Section::Effect) {
                    return false;
                }
                --sectionDepth;
                break;
            case CmdKind::DrawParticles: {
                if (index >= list.particles.size()) {
                    return false;
                }
                const ParticlesCmd& c = list.particles[index];
                if (!rangeOk(c.instanceOffset, c.count, list.particleInstances.size()) ||
                    !sourceOk(c.source)) {
                    return false;
                }
                break;
            }
            case CmdKind::DrawImageMesh: {
                if (index >= list.imageMeshes.size()) {
                    return false;
                }
                const ImageMeshCmd& c = list.imageMeshes[index];
                if (!rangeOk(c.vertexOffset, c.vertexCount, list.meshVertices.size()) ||
                    !rangeOk(c.indexOffset, c.indexCount, list.meshIndices.size()) || !sourceOk(c.source)) {
                    return false;
                }
                break;
            }
            default:
                return false;  // Not a CmdKind this build knows.
        }
    }
    return sectionDepth == 0 && clipDepth == 0 && projectionDepth == 0;
}

// -------------------------------------------------------------------------------------------
// T-343: scene runs (docs/design/23-host-integration.md §23.aa.2).
// -------------------------------------------------------------------------------------------

void scanSceneRuns(const CommandList& list, std::vector<SceneRun>& out) {
    out.clear();
    const size_t count = list.commands.size();
    size_t bandStart = 0;
    int depth = 0;  // Layer / Mask / Effect nesting
    auto closeBand = [&](size_t end) {
        if (end > bandStart) {
            out.push_back(SceneRun{static_cast<uint32_t>(bandStart), static_cast<uint32_t>(end), false});
        }
    };
    for (size_t i = 0; i < count; ++i) {
        const Command& cmd = list.commands[i];
        const bool opens = cmd.kind == CmdKind::PushLayer || cmd.kind == CmdKind::PushMask ||
                           cmd.kind == CmdKind::PushEffect;
        const bool closes =
            cmd.kind == CmdKind::PopLayer || cmd.kind == CmdKind::PopMask || cmd.kind == CmdKind::PopEffect;
        if (depth == 0 && opens && (cmd.flags[0] & kCommandSceneRead) != 0 && cmd.kind != CmdKind::PushMask) {
            // A scene-reading subtree: find its matching pop (a valid list always has one).
            size_t end = count;
            int inner = 0;
            for (size_t j = i; j < count; ++j) {
                const CmdKind k = list.commands[j].kind;
                if (k == CmdKind::PushLayer || k == CmdKind::PushMask || k == CmdKind::PushEffect) {
                    ++inner;
                } else if (k == CmdKind::PopLayer || k == CmdKind::PopMask || k == CmdKind::PopEffect) {
                    if (--inner == 0) {
                        end = j + 1;
                        break;
                    }
                }
            }
            closeBand(i);
            out.push_back(SceneRun{static_cast<uint32_t>(i), static_cast<uint32_t>(end), true});
            bandStart = end;
            i = end - 1;
            continue;
        }
        if (opens) {
            ++depth;
        } else if (closes && depth > 0) {
            --depth;
        }
    }
    closeBand(count);
}

}  // namespace xt

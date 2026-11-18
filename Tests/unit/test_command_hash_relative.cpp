#include <cstdint>
#include <vector>

#include "damage_internal.h"
#include "doctest.h"
#include "xt/command_list.h"
#include "xt/types.h"

/// T-721 (Docs/design/10-backends.md §10.16 W2 item 1): hashCommandRelative(), the per-command hash
/// with positions taken relative to an origin -- the effect cache key that survives a translation.

using namespace xt;

namespace {

void push(CommandList& l, CmdKind kind, size_t index) {
    l.commands.push_back(Command{kind, {}, static_cast<uint32_t>(index)});
}

PathRef addSquare(CommandList& l, float side) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(l.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(l.points.size());
    const float pts[8] = {0, 0, side, 0, side, side, 0, side};
    for (int i = 0; i < 4; ++i) {
        l.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        l.points.push_back(pts[i * 2]);
        l.points.push_back(pts[i * 2 + 1]);
    }
    l.verbs.push_back(PathVerb::Close);
    ref.verbCount = static_cast<uint32_t>(l.verbs.size()) - ref.verbOffset;
    ref.pointCount = static_cast<uint32_t>(l.points.size()) - ref.pointOffset;
    return ref;
}

/// One command of every kind, as a node at `at` (a point of the plane and of the screen: no 3D
/// space) would produce them: transforms translated to `at`, screen boxes starting there.
CommandList everyKind(Vec2 at) {
    CommandList l;
    const Mat2x3 world = mul(translation(at), scaling(Vec2{1.5f, 0.75f}));
    const Rect box{at.x - 4, at.y + 2, 60, 40};

    FillCmd fill{};
    fill.xf = world;
    fill.path = addSquare(l, 20);
    fill.paint.kind = PaintKind::Solid;
    fill.paint.color = Color{1, 0, 0, 1};
    fill.antialias = 1;
    l.fills.push_back(fill);
    push(l, CmdKind::FillPath, 0);

    StrokeCmd stroke{};
    stroke.xf = world;
    stroke.path = addSquare(l, 30);
    stroke.paint.kind = PaintKind::Solid;
    stroke.paint.color = Color{0, 1, 0, 1};
    stroke.width = 2;
    stroke.miterLimit = 4;
    l.strokes.push_back(stroke);
    push(l, CmdKind::StrokePath, 0);

    ImageCmd image{};
    image.xf = world;
    image.asset = 3;
    image.dst = Rect{0, 0, 16, 16};
    image.srcUv = Rect{0, 0, 1, 1};
    image.opacity = 1;
    l.images.push_back(image);
    push(l, CmdKind::DrawImage, 0);

    GlyphsCmd glyphs{};
    glyphs.xf = world;
    glyphs.size = 14;
    glyphs.color = Color{1, 1, 1, 1};
    glyphs.glyphOffset = 0;
    glyphs.glyphCount = 2;
    l.glyphs = {7, 9};
    l.glyphPos = {Vec2{0, 12}, Vec2{8, 12}};
    l.glyphRuns.push_back(glyphs);
    push(l, CmdKind::DrawGlyphs, 0);

    ClipRectCmd clipRect{};
    clipRect.xf = world;
    clipRect.rect = Rect{0, 0, 30, 30};
    l.clipRects.push_back(clipRect);
    push(l, CmdKind::PushClipRect, 0);

    ClipPathCmd clipPath{};
    clipPath.xf = world;
    clipPath.path = addSquare(l, 25);
    l.clipPaths.push_back(clipPath);
    push(l, CmdKind::PushClipPath, 0);

    LayerCmd layer{};
    layer.opacity = 0.5f;
    layer.blend = BlendMode::Normal;
    layer.boundsHint = box;
    l.layers.push_back(layer);
    push(l, CmdKind::PushLayer, 0);

    MaskCmd mask{};
    mask.mode = MaskMode::Alpha;
    mask.boundsHint = box;
    l.masks.push_back(mask);
    push(l, CmdKind::PushMask, 0);
    push(l, CmdKind::EndMaskContent, 0);
    push(l, CmdKind::PopMask, 0);
    push(l, CmdKind::PopLayer, 0);

    ProjectionCmd projection{};
    projection.viewProj = mat4Identity();
    projection.rootXf = world;
    projection.boxRect = Rect{0, 0, 50, 50};
    projection.boundsHint = box;
    l.projections.push_back(projection);
    push(l, CmdKind::PushProjection, 0);

    Transform3DCmd transform{};
    transform.world3 = mat4RotateY(20.0f);
    l.transforms3.push_back(transform);
    push(l, CmdKind::PushTransform3D, 0);
    push(l, CmdKind::PopTransform3D, 0);
    push(l, CmdKind::PopProjection, 0);

    ImageQuadCmd quad{};
    quad.asset = 4;
    quad.p[0] = Vec2{at.x, at.y};
    quad.p[1] = Vec2{at.x + 20, at.y + 1};
    quad.p[2] = Vec2{at.x + 21, at.y + 19};
    quad.p[3] = Vec2{at.x - 1, at.y + 20};
    quad.srcUv = Rect{0, 0, 1, 1};
    quad.opacity = 1;
    l.imageQuads.push_back(quad);
    push(l, CmdKind::DrawImageQuad, 0);

    EffectCmd effect{};
    effect.effectOffset = 0;
    effect.effectCount = 1;
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.params[0] = blur.params[1] = 3;
    l.effects.push_back(blur);
    effect.xf = world;
    effect.boundsHint = box;
    effect.contentBounds = Rect{at.x, at.y + 6, 52, 32};
    l.effectCmds.push_back(effect);
    push(l, CmdKind::PushEffect, 0);
    push(l, CmdKind::PopEffect, 0);

    ParticlesCmd particles{};
    particles.xf = world;
    particles.texture = kInvalidAsset;
    particles.instanceOffset = 0;
    particles.count = 1;
    ParticleInstance instance{};
    instance.x = 5;
    instance.y = 6;
    instance.w = instance.h = 4;
    instance.rgba = Color{1, 1, 1, 1};
    l.particleInstances.push_back(instance);
    l.particles.push_back(particles);
    push(l, CmdKind::DrawParticles, 0);

    ImageMeshCmd mesh{};
    mesh.asset = 5;
    mesh.vertexOffset = 0;
    mesh.vertexCount = 3;
    mesh.indexOffset = 0;
    mesh.indexCount = 3;
    mesh.opacity = 1;
    l.meshVertices = {MeshVertex{{at.x, at.y}, {0, 0}}, MeshVertex{{at.x + 30, at.y + 2}, {1, 0}},
                      MeshVertex{{at.x + 4, at.y + 28}, {0, 1}}};
    l.meshIndices = {0, 1, 2};
    l.imageMeshes.push_back(mesh);
    push(l, CmdKind::DrawImageMesh, 0);

    BoxShadowCmd shadow{};
    shadow.xf = world;
    shadow.rect = Rect{0, 0, 40, 30};
    shadow.blur = 6;
    shadow.color = Color{0, 0, 0, 0.5f};
    l.boxShadows.push_back(shadow);
    push(l, CmdKind::DrawBoxShadow, 0);

    push(l, CmdKind::PopClip, 0);
    push(l, CmdKind::PopClip, 0);
    return l;
}

CommandHashOrigin originAt(Vec2 at, float steps = 0.0f) {
    CommandHashOrigin o;
    o.plane = at;
    o.screen = at;
    o.planeSteps = o.screenSteps = steps;
    o.linearSteps = steps > 0.0f ? 256.0f : 0.0f;
    return o;
}

std::vector<uint64_t> relativeHashes(const CommandList& l, const CommandHashOrigin& o) {
    std::vector<uint64_t> out;
    for (uint32_t i = 0; i < l.commands.size(); ++i) {
        out.push_back(hashCommandRelative(l, i, o));
    }
    return out;
}

}  // namespace

TEST_CASE("damage: T-721 hashCommandRelative with a zero origin is hashCommandList, for every command kind") {
    const CommandList l = everyKind(Vec2{123.25f, -40.5f});
    std::vector<uint64_t> plain;
    hashCommandList(l, plain);
    const std::vector<uint64_t> relative = relativeHashes(l, CommandHashOrigin{});
    REQUIRE(plain.size() == l.commands.size());
    REQUIRE(relative.size() == plain.size());
    for (size_t i = 0; i < plain.size(); ++i) {
        CAPTURE(i);
        CHECK(relative[i] == plain[i]);
    }
    // Past the list: the hash of nothing, never a read out of bounds.
    CHECK(hashCommandRelative(l, static_cast<uint32_t>(l.commands.size()), CommandHashOrigin{}) ==
          hashCommandRelative(CommandList{}, 0, CommandHashOrigin{}));
}

TEST_CASE("damage: T-721 hashCommandRelative is the same wherever the commands are, exactly and rounded") {
    const Vec2 a{100, 60}, b{357, 12};  // whole pixels: the relative floats are exact
    const CommandList la = everyKind(a);
    const CommandList lb = everyKind(b);
    const std::vector<uint64_t> ha = relativeHashes(la, originAt(a));
    const std::vector<uint64_t> hb = relativeHashes(lb, originAt(b));
    std::vector<uint64_t> plainA, plainB;
    hashCommandList(la, plainA);
    hashCommandList(lb, plainB);
    REQUIRE(ha.size() == hb.size());
    for (size_t i = 0; i < ha.size(); ++i) {
        CAPTURE(i);
        CHECK(ha[i] == hb[i]);
        const CmdKind kind = la.commands[i].kind;
        const bool placed = kind != CmdKind::PopClip && kind != CmdKind::PopLayer &&
                            kind != CmdKind::PopMask && kind != CmdKind::EndMaskContent &&
                            kind != CmdKind::PopProjection && kind != CmdKind::PopTransform3D &&
                            kind != CmdKind::PopEffect && kind != CmdKind::PushTransform3D;
        CHECK((plainA[i] != plainB[i]) == placed);  // the plain hash does see the move
    }
    // A fraction of a pixel on, the positions differ in their last bits: the rounded form is the
    // same, to 1/16 of a unit.
    const Vec2 c{100.3f, 60.7f};
    const CommandList lc = everyKind(c);
    const std::vector<uint64_t> ra = relativeHashes(la, originAt(a, 16.0f));
    const std::vector<uint64_t> rc = relativeHashes(lc, originAt(c, 16.0f));
    for (size_t i = 0; i < ra.size(); ++i) {
        CAPTURE(i);
        CHECK(ra[i] == rc[i]);
    }
}

TEST_CASE("damage: T-721 hashCommandRelative sees everything that is not the place") {
    const Vec2 at{100, 60};
    const CommandList base = everyKind(at);
    const std::vector<uint64_t> h0 = relativeHashes(base, originAt(at, 16.0f));
    auto changed = [&](const CommandList& l, CmdKind kind) {
        const std::vector<uint64_t> h = relativeHashes(l, originAt(at, 16.0f));
        bool onlyThatKind = true, thatKind = false;
        for (size_t i = 0; i < h.size(); ++i) {
            if (h[i] != h0[i]) {
                thatKind = thatKind || l.commands[i].kind == kind;
                onlyThatKind = onlyThatKind && l.commands[i].kind == kind;
            }
        }
        return thatKind && onlyThatKind;
    };
    {
        CommandList l = base;  // a position inside: half a unit is beyond the rounding
        l.fills[0].xf.e += 0.5f;
        CHECK(changed(l, CmdKind::FillPath));
    }
    {
        CommandList l = base;  // the linear part
        l.images[0].xf.a = 1.25f;
        CHECK(changed(l, CmdKind::DrawImage));
    }
    {
        CommandList l = base;  // a box's size
        l.layers[0].boundsHint.w += 1;
        CHECK(changed(l, CmdKind::PushLayer));
    }
    {
        CommandList l = base;  // an effect parameter (the pool)
        l.effects[0].params[0] = 4;
        CHECK(changed(l, CmdKind::PushEffect));
    }
    {
        CommandList l = base;  // the effect's own scale, beyond the 1/256 it is rounded to
        l.effectCmds[0].xf.a += 0.01f;
        CHECK(changed(l, CmdKind::PushEffect));
    }
    {
        CommandList l = base;  // one mesh vertex
        l.meshVertices[1].p.y += 1;
        CHECK(changed(l, CmdKind::DrawImageMesh));
    }
    {
        CommandList l = base;  // one corner of an image quad
        l.imageQuads[0].p[2].x += 1;
        CHECK(changed(l, CmdKind::DrawImageQuad));
    }
    {
        // The lazy "whole viewport" box (empty) is not a place: it hashes as itself.
        CommandList l = base;
        l.layers[0].boundsHint = Rect{};
        const uint64_t here = hashCommandRelative(l, 6, originAt(at, 16.0f));
        const uint64_t there = hashCommandRelative(l, 6, originAt(Vec2{5, 5}, 16.0f));
        REQUIRE(l.commands[6].kind == CmdKind::PushLayer);
        CHECK(here == there);
    }
}

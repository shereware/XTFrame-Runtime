#include <cstdint>
#include <string>
#include <vector>

#include "damage_internal.h"
#include "doctest.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/damage.h"
#include "xt/node.h"
#include "xt/shader_table.h"
#include "xt/types.h"

/// validateCommandList (docs/tasks/T-263a.md E1) and the CommandListSimplifier's handling of a list
/// that fails it (B10).

using namespace xt;

namespace {

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.insert(cl.verbs.end(),
                    {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Line, PathVerb::Close});
    const float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void push(CommandList& cl, CmdKind kind, uint32_t index = 0) {
    cl.commands.push_back(Command{kind, {}, index});
}

void addFill(CommandList& cl) {
    FillCmd f{};
    f.path = addRectPath(cl, 0, 0, 10, 10);
    f.paint.kind = PaintKind::Solid;
    f.paint.color = Color{1, 0, 0, 1};
    cl.fills.push_back(f);
    push(cl, CmdKind::FillPath, static_cast<uint32_t>(cl.fills.size() - 1));
}

/// A valid list with one command of every kind, nested the way the Painter nests them.
CommandList validList() {
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    addFill(cl);

    StrokeCmd s{};
    s.path = addRectPath(cl, 2, 2, 8, 8);
    s.paint.kind = PaintKind::Linear;
    s.paint.stopOffset = 0;
    s.paint.stopCount = 2;
    cl.stops = {GradientStop{0, Color{0, 0, 0, 1}}, GradientStop{1, Color{1, 1, 1, 1}}};
    s.width = 2;
    s.dashOffset = 0;
    s.dashCount = 2;
    cl.dashes = {3, 1};
    cl.strokes.push_back(s);

    cl.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, cl.viewport});
    cl.masks.push_back(MaskCmd{MaskMode::Alpha, cl.viewport});
    cl.clipRects.push_back(ClipRectCmd{Mat2x3{}, Rect{0, 0, 32, 32}});
    ClipPathCmd cp{};
    cp.path = addRectPath(cl, 4, 4, 20, 20);
    cl.clipPaths.push_back(cp);
    cl.images.push_back(ImageCmd{});
    GlyphsCmd g{};
    g.glyphOffset = 0;
    g.glyphCount = 2;
    cl.glyphs = {65, 66};
    cl.glyphPos = {Vec2{0, 10}, Vec2{8, 10}};
    cl.glyphRuns.push_back(g);
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    cl.effects.push_back(blur);
    EffectCmd ec{};
    ec.effectOffset = 0;
    ec.effectCount = 1;
    cl.effectCmds.push_back(ec);
    cl.projections.push_back(ProjectionCmd{});
    cl.transforms3.push_back(Transform3DCmd{});
    cl.imageQuads.push_back(ImageQuadCmd{});
    ParticlesCmd pc{};
    pc.instanceOffset = 0;
    pc.count = 1;
    cl.particles.push_back(pc);
    cl.particleInstances.push_back(ParticleInstance{});
    ImageMeshCmd mc{};
    mc.vertexOffset = 0;
    mc.vertexCount = 3;
    mc.indexOffset = 0;
    mc.indexCount = 3;
    cl.imageMeshes.push_back(mc);
    cl.meshVertices.resize(3);
    cl.meshIndices = {0, 1, 2};

    push(cl, CmdKind::PushLayer);
    push(cl, CmdKind::PushClipRect);
    push(cl, CmdKind::StrokePath);
    push(cl, CmdKind::PushMask);
    push(cl, CmdKind::PushClipPath);
    push(cl, CmdKind::DrawImage);
    push(cl, CmdKind::PopClip);
    push(cl, CmdKind::EndMaskContent);
    push(cl, CmdKind::PushEffect);
    push(cl, CmdKind::DrawGlyphs);
    push(cl, CmdKind::DrawParticles);
    push(cl, CmdKind::PopEffect);
    push(cl, CmdKind::PopMask);
    push(cl, CmdKind::PopClip);
    push(cl, CmdKind::PopLayer);
    push(cl, CmdKind::PushProjection);
    push(cl, CmdKind::PushTransform3D);
    push(cl, CmdKind::DrawImageQuad);
    push(cl, CmdKind::PopTransform3D);
    push(cl, CmdKind::PopProjection);
    push(cl, CmdKind::DrawImageMesh);
    return cl;
}

}  // namespace

TEST_CASE("validateCommandList: a well-formed list of every kind passes") {
    CHECK(validateCommandList(validList()));
    CHECK(validateCommandList(CommandList{}));
}

TEST_CASE("validateCommandList: every index past its pool fails") {
    const CmdKind kinds[] = {CmdKind::FillPath,     CmdKind::StrokePath,    CmdKind::DrawImage,
                             CmdKind::DrawGlyphs,   CmdKind::DrawImageQuad, CmdKind::DrawParticles,
                             CmdKind::DrawImageMesh};
    for (CmdKind kind : kinds) {
        CAPTURE(static_cast<int>(kind));
        CommandList cl = validList();
        push(cl, kind, 1000);
        CHECK(!validateCommandList(cl));
    }
    // A push with a bad index fails even when its pop is there.
    const CmdKind pushes[][2] = {{CmdKind::PushClipRect, CmdKind::PopClip},
                                 {CmdKind::PushClipPath, CmdKind::PopClip},
                                 {CmdKind::PushLayer, CmdKind::PopLayer},
                                 {CmdKind::PushEffect, CmdKind::PopEffect},
                                 {CmdKind::PushProjection, CmdKind::PopProjection}};
    for (const auto& pair : pushes) {
        CAPTURE(static_cast<int>(pair[0]));
        CommandList cl = validList();
        push(cl, pair[0], 1000);
        push(cl, pair[1]);
        CHECK(!validateCommandList(cl));
    }
    CommandList unknown = validList();
    push(unknown, static_cast<CmdKind>(200));
    CHECK(!validateCommandList(unknown));
}

TEST_CASE("validateCommandList: offset + count ranges are checked in 64 bits") {
    SUBCASE("verbs and points disagree") {
        CommandList cl = validList();
        cl.fills[0].path.pointCount = 6;  // Move + 3 Line need 8 floats
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("an unknown verb") {
        CommandList cl = validList();
        cl.verbs[1] = static_cast<PathVerb>(9);
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("a point range that wraps in 32 bits") {
        CommandList cl = validList();
        cl.fills[0].path.pointOffset = 0xFFFFFFFCu;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("a clip path's verb range") {
        CommandList cl = validList();
        cl.clipPaths[0].path.verbCount = 100;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("effectCount = 0xFFFFFFFF") {
        CommandList cl = validList();
        cl.effectCmds[0].effectCount = 0xFFFFFFFFu;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("gradient stops") {
        CommandList cl = validList();
        cl.strokes[0].paint.stopCount = 3;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("an effect's stops") {
        CommandList cl = validList();
        cl.effects[0].stopOffset = 1;
        cl.effects[0].stopCount = 2;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("dashes") {
        CommandList cl = validList();
        cl.strokes[0].dashOffset = 1;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("glyph positions shorter than the glyphs") {
        CommandList cl = validList();
        cl.glyphPos.pop_back();
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("particle instances") {
        CommandList cl = validList();
        cl.particles[0].count = 0xFFFFFFFFu;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("mesh vertices and indices") {
        CommandList cl = validList();
        cl.imageMeshes[0].indexOffset = 1;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("shader text and params") {
        CommandList cl = validList();
        ShaderRef ref{};
        ref.sourceOffset = 0;
        ref.sourceLength = 4;
        cl.shaders.push_back(ref);
        CHECK(!validateCommandList(cl));
        cl.shaderText = {'a', 'b', 'c', 'd'};
        CHECK(validateCommandList(cl));
        cl.shaders[0].paramCount = 1;
        CHECK(!validateCommandList(cl));
    }
    SUBCASE("an empty range is fine at any offset") {
        CommandList cl = validList();
        cl.fills[0].paint.stopOffset = 0xFFFFFFFFu;
        cl.fills[0].paint.stopCount = 0;
        CHECK(validateCommandList(cl));
    }
}

TEST_CASE("validateCommandList: pops of an empty stack, crossed sections and open sections fail") {
    const std::vector<std::vector<CmdKind>> bad = {
        {CmdKind::PopClip},
        {CmdKind::PopLayer},
        {CmdKind::PopMask},
        {CmdKind::EndMaskContent},
        {CmdKind::PopEffect},
        {CmdKind::PopProjection},
        {CmdKind::PushTransform3D},
        {CmdKind::PopTransform3D},
        {CmdKind::PushLayer},
        {CmdKind::PushClipRect},
        {CmdKind::PushProjection},
        {CmdKind::PushMask, CmdKind::PopMask},  // no EndMaskContent
        {CmdKind::PushMask, CmdKind::EndMaskContent, CmdKind::EndMaskContent, CmdKind::PopMask},
        {CmdKind::PushLayer, CmdKind::PushEffect, CmdKind::PopLayer, CmdKind::PopEffect},
        {CmdKind::PushMask, CmdKind::EndMaskContent, CmdKind::PushMask, CmdKind::PopMask, CmdKind::PopMask},
        {CmdKind::PushEffect, CmdKind::PopLayer},
    };
    for (size_t i = 0; i < bad.size(); ++i) {
        CAPTURE(i);
        CommandList cl = validList();
        for (CmdKind kind : bad[i]) {
            push(cl, kind);
        }
        CHECK(!validateCommandList(cl));
    }
}

TEST_CASE("validateCommandList: sections nested deeper than the fixed array are still matched") {
    for (uint32_t depth : {63u, 64u, 65u, 200u}) {
        CAPTURE(depth);
        CommandList cl = validList();
        for (uint32_t i = 0; i < depth; ++i) {
            push(cl,
                 i % 3 == 0 ? CmdKind::PushMask : (i % 3 == 1 ? CmdKind::PushEffect : CmdKind::PushLayer));
            if (i % 3 == 0) {
                push(cl, CmdKind::EndMaskContent);
            }
        }
        CommandList ok = cl;
        for (uint32_t i = depth; i-- > 0;) {
            push(ok, i % 3 == 0 ? CmdKind::PopMask : (i % 3 == 1 ? CmdKind::PopEffect : CmdKind::PopLayer));
        }
        CHECK(validateCommandList(ok));

        // The same pops with the two innermost swapped (when they differ) must fail.
        CommandList crossed = cl;
        for (uint32_t n = 0; n < depth; ++n) {
            uint32_t i = depth - 1 - n;
            if (n == 0 && depth >= 2) {
                i = depth - 2;
            } else if (n == 1 && depth >= 2) {
                i = depth - 1;
            }
            push(crossed,
                 i % 3 == 0 ? CmdKind::PopMask : (i % 3 == 1 ? CmdKind::PopEffect : CmdKind::PopLayer));
        }
        CHECK(!validateCommandList(crossed));
    }
}

// T-263a B10: the simplifier hands an invalid list back untouched (it would otherwise pop its
// opacity stack past the root on an unmatched PopLayer).
TEST_CASE("CommandListSimplifier: T-263a an invalid list is returned unsimplified") {
    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1;
    addFill(cl);
    push(cl, CmdKind::PopLayer);
    addFill(cl);
    std::string before;
    dumpCommandList(cl, before);

    CommandListSimplifier simplifier;
    SimplifyOptions options;
    const SimplifyStats stats = simplifier.simplify(cl, options, nullptr);
    std::string after;
    dumpCommandList(cl, after);
    CHECK(after == before);
    CHECK(stats.flattenedCommands == 0);

    // A valid list is still simplified (layers folded into opacity).
    CommandList valid;
    valid.viewport = cl.viewport;
    valid.devicePixelRatio = 1;
    valid.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, valid.viewport});
    push(valid, CmdKind::PushLayer);
    addFill(valid);
    push(valid, CmdKind::PopLayer);
    simplifier.simplify(valid, options, nullptr);
    REQUIRE(valid.commands.size() == 1);
    CHECK(valid.fills[0].paint.color.a == doctest::Approx(0.5f));
}

// =================================================================================================
// T-263d: CommandList::sources and the `source` of the asset-carrying commands.
// =================================================================================================

namespace {

/// Two IrView addresses that are never dereferenced: validating, dumping, hashing and simplifying
/// a list only count or copy its source table.
const IrView* fakeIr(int i) {
    static const char storage[2] = {0, 0};
    return reinterpret_cast<const IrView*>(&storage[i]);
}

/// validList() with a table of two sources and a ShaderRef, every asset command stamped `source`.
CommandList sourcedList(uint16_t source) {
    CommandList cl = validList();
    cl.sources = {CommandSource{fakeIr(0), 11}, CommandSource{fakeIr(1), 12}};
    cl.shaders.push_back(ShaderRef{});
    cl.images[0].source = source;
    cl.imageQuads[0].source = source;
    cl.particles[0].source = source;
    cl.imageMeshes[0].source = source;
    cl.effectCmds[0].source = source;
    cl.shaders[0].source = source;
    return cl;
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("validateCommandList: T-263d a source outside CommandList::sources fails") {
    // A list that names no source ignores the field: the call IR is every command's.
    CommandList none = validList();
    none.images[0].source = 7;
    CHECK(validateCommandList(none));
    CHECK(sourceIr(none, 7, fakeIr(0)) == fakeIr(0));
    CHECK(sourceIr(none, 0, nullptr) == nullptr);

    const CommandList cl = sourcedList(1);
    CHECK(validateCommandList(cl));
    CHECK(sourceIr(cl, 0, nullptr) == fakeIr(0));
    CHECK(sourceIr(cl, 1, fakeIr(0)) == fakeIr(1));
    CHECK(sourceIr(cl, 2, fakeIr(0)) == nullptr);  // not the call IR: the table decides
    {
        CommandList bad = cl;
        bad.images[0].source = 2;
        CHECK_FALSE(validateCommandList(bad));
    }
    {
        CommandList bad = cl;
        bad.imageQuads[0].source = 2;
        CHECK_FALSE(validateCommandList(bad));
    }
    {
        CommandList bad = cl;
        bad.particles[0].source = 0xFFFF;
        CHECK_FALSE(validateCommandList(bad));
    }
    {
        CommandList bad = cl;
        bad.imageMeshes[0].source = 2;
        CHECK_FALSE(validateCommandList(bad));
    }
    {
        CommandList bad = cl;
        bad.effectCmds[0].source = 2;
        CHECK_FALSE(validateCommandList(bad));
    }
    {
        CommandList bad = cl;
        bad.shaders[0].source = 2;
        CHECK_FALSE(validateCommandList(bad));
    }
    // clear() empties the table.
    CommandList cleared = cl;
    cleared.clear();
    CHECK(cleared.sources.empty());
}

TEST_CASE("dumpCommandList: T-263d asset commands print their source only when the list names sources") {
    CommandList cl = sourcedList(1);
    std::string named;
    dumpCommandList(cl, named);
    // DrawImage, DrawImageQuad, PushEffect, DrawParticles, DrawImageMesh.
    CHECK(countOf(named, " source=1") == 5);
    CHECK(countOf(named, " source=") == 5);

    cl.sources.clear();
    std::string plain;
    dumpCommandList(cl, plain);
    CHECK(countOf(plain, "source=") == 0);
    // Nothing else differs.
    std::string stripped = named;
    for (size_t at = stripped.find(" source=1"); at != std::string::npos; at = stripped.find(" source=1")) {
        stripped.erase(at, 9);
    }
    CHECK(stripped == plain);
}

TEST_CASE("damage: T-263d a command's source is part of its hash; a base list's sources are never read") {
    const CommandList a = sourcedList(0);
    std::vector<uint64_t> base;
    hashCommandList(a, base);
    auto changed = [&](const CommandList& b) {
        std::vector<uint64_t> h;
        hashCommandList(b, h);
        REQUIRE(h.size() == base.size());
        int n = 0;
        for (size_t i = 0; i < h.size(); ++i) {
            n += h[i] != base[i] ? 1 : 0;
        }
        return n;
    };
    CHECK(changed(a) == 0);
    {
        CommandList b = a;
        b.images[0].source = 1;
        CHECK(changed(b) == 1);
    }
    {
        CommandList b = a;
        b.imageQuads[0].source = 1;
        CHECK(changed(b) == 1);
    }
    {
        CommandList b = a;
        b.particles[0].source = 1;
        CHECK(changed(b) == 1);
    }
    {
        CommandList b = a;
        b.imageMeshes[0].source = 1;
        CHECK(changed(b) == 1);
    }
    {
        CommandList b = a;
        b.effectCmds[0].source = 1;
        CHECK(changed(b) == 1);
    }

    // The previous frame's table may name IRs that are gone: the diff runs with a base whose
    // pointers cannot be read, and reports the one command that changed.
    CommandList prev = a;
    prev.images[0].dst = Rect{4, 4, 8, 8};
    prev.sources = {CommandSource{reinterpret_cast<const IrView*>(static_cast<uintptr_t>(16)), 1},
                    CommandSource{reinterpret_cast<const IrView*>(static_cast<uintptr_t>(32)), 2}};
    CommandList cur = a;
    cur.images[0].dst = prev.images[0].dst;
    CommandList unchanged = cur;
    cur.images[0].source = 1;
    DamageList damage;
    computeDamage(prev, cur, Rect{0, 0, 64, 64}, damage);
    CHECK((damage.full || !damage.rects.empty()));
    DamageList same;
    computeDamage(prev, unchanged, Rect{0, 0, 64, 64}, same);
    CHECK_FALSE(same.full);
    CHECK(same.rects.empty());
}

TEST_CASE("CommandListSimplifier: T-263d every pass keeps the source table and the commands' sources") {
    for (bool flatten : {false, true}) {
        CAPTURE(flatten);
        CommandList cl = sourcedList(1);
        SimplifyOptions options;
        options.flatten3D = flatten;
        options.strokes = false;
        options.gradients = false;
        options.layers = false;
        options.masks = false;
        options.clipPaths = false;
        options.glyphsToPaths = false;
        options.effects = false;
        options.particles = false;
        CommandListSimplifier simplifier;
        simplifier.simplify(cl, options, nullptr);
        REQUIRE(cl.sources.size() == 2);
        CHECK(cl.sources[0].ir == fakeIr(0));
        CHECK(cl.sources[1].ir == fakeIr(1));
        CHECK(cl.sources[1].loadSerial == 12);
        REQUIRE(cl.images.size() == 1);
        CHECK(cl.images[0].source == 1);
        REQUIRE(cl.particles.size() == 1);
        CHECK(cl.particles[0].source == 1);
        REQUIRE(cl.imageMeshes.size() == 1);
        CHECK(cl.imageMeshes[0].source == 1);
        REQUIRE(cl.effectCmds.size() == 1);
        CHECK(cl.effectCmds[0].source == 1);
        if (!flatten) {
            REQUIRE(cl.imageQuads.size() == 1);
            CHECK(cl.imageQuads[0].source == 1);
        }
        CHECK(validateCommandList(cl));
    }
}

TEST_CASE("ShaderTable::copyTo: T-263d one ShaderRef per (source, asset)") {
    ShaderTable table;
    table.text = "abc";
    ShaderRef ref{};
    ref.asset = 0;
    ref.sourceOffset = 0;
    ref.sourceLength = 1;
    ref.glslOffset = 1;
    ref.glslLength = 1;
    ref.namesOffset = 2;
    ref.namesLength = 1;
    table.refs.push_back(ref);

    CommandList out;
    table.copyTo(0, 0, out);
    table.copyTo(0, 0, out);
    REQUIRE(out.shaders.size() == 1);
    CHECK(out.shaders[0].source == 0);
    // Another document's shader 0 is another entry, with its own text.
    table.copyTo(0, 1, out);
    table.copyTo(0, 1, out);
    table.copyTo(0, 0, out);
    REQUIRE(out.shaders.size() == 2);
    CHECK(out.shaders[1].asset == 0);
    CHECK(out.shaders[1].source == 1);
    CHECK(out.shaders[1].sourceOffset == 3);
    CHECK(out.shaderText.size() == 6);
    table.copyTo(5, 1, out);  // unknown asset: nothing
    CHECK(out.shaders.size() == 2);
}

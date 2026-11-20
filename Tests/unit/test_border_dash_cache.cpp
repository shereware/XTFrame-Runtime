#include <string>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-737 (Docs/tasks/T-737.md A): the Painter's dashed / dotted border geometry cache. The command
/// list must be byte-identical with the cache on and off; the counters show what it held and served.

using namespace xt;

namespace {

/// Every byte of `list` a backend reads for its FillPaths: the debug dump, the raw verb and point
/// pools, and each fill's transform, paint, rule and antialias flag.
std::string listBytes(const CommandList& list) {
    std::string out;
    dumpCommandList(list, out);
    out.append(reinterpret_cast<const char*>(list.verbs.data()), list.verbs.size() * sizeof(PathVerb));
    out.append(reinterpret_cast<const char*>(list.points.data()), list.points.size() * sizeof(float));
    for (const Command& c : list.commands) {
        out.push_back(static_cast<char>(c.kind));
        out.append(reinterpret_cast<const char*>(&c.index), sizeof c.index);
    }
    for (const FillCmd& f : list.fills) {
        const float xf[6] = {f.xf.a, f.xf.b, f.xf.c, f.xf.d, f.xf.e, f.xf.f};
        out.append(reinterpret_cast<const char*>(xf), sizeof(xf));
        const float c[4] = {f.paint.color.r, f.paint.color.g, f.paint.color.b, f.paint.color.a};
        out.append(reinterpret_cast<const char*>(c), sizeof(c));
        out.push_back(static_cast<char>(f.paint.kind));
        out.push_back(static_cast<char>(f.rule));
        out.push_back(static_cast<char>(f.antialias));
        const uint32_t ref[4] = {f.path.verbOffset, f.path.verbCount, f.path.pointOffset, f.path.pointCount};
        out.append(reinterpret_cast<const char*>(ref), sizeof(ref));
    }
    return out;
}

NodeId makeElement(NodeArena& arena, NodeId parent) {
    NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    return id;
}

/// A child box at (x, y), w x h, with `widths` border, the four side styles, one radius on every
/// corner and four side colors.
NodeId addBox(NodeArena& arena, NodeId parent, float x, float y, float w, float h, Edges widths,
              const uint8_t styles[4], float radius, bool fourColors) {
    NodeId n = makeElement(arena, parent);
    ElementData& data = arena.element(n);
    data.box.x = x;
    data.box.y = y;
    arena.common(n).translate = Vec2{x, y};  // what layout would set
    data.box.width = w;
    data.box.height = h;
    data.box.border = widths;
    data.style.base[prop::BorderColor] = Value::ofColor(Color{0.2f, 0.3f, 0.4f, 1});
    for (int i = 0; i < 4; ++i) {
        data.style.borderStyle[i] = styles[i];
    }
    if (radius > 0.0f) {
        for (PropId p = prop::BorderRadiusTL; p <= prop::BorderRadiusBL; ++p) {
            data.style.base[p] = Value::ofLength(radius, LengthUnit::Px);
        }
    }
    if (fourColors) {
        const Color colors[4] = {Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, Color{0, 0, 1, 1}, Color{1, 1, 0, 1}};
        for (int i = 0; i < 4; ++i) {
            data.style.base[prop::BorderTopColor + i] = Value::ofColor(colors[i]);
        }
        data.style.borderColorSides = 0xF;
    }
    return n;
}

constexpr uint8_t kAllDashed[4] = {kBorderStyleDashed, kBorderStyleDashed, kBorderStyleDashed,
                                   kBorderStyleDashed};
constexpr uint8_t kAllDotted[4] = {kBorderStyleDotted, kBorderStyleDotted, kBorderStyleDotted,
                                   kBorderStyleDotted};
constexpr uint8_t kMixed[4] = {kBorderStyleDashed, kBorderStyleDotted, kBorderStyleSolid, kBorderStyleDashed};

/// A 400 x 300 root with three bordered children: square dashed (4 sides), rounded dotted with four
/// widths and colors (4 sides), rounded mixed dashed / dotted / solid / dashed (3 non-solid sides).
struct MixedScene {
    NodeArena arena;
    NodeId root = kInvalidNode;
    NodeId square = kInvalidNode;
    NodeId dotted = kInvalidNode;
    NodeId mixed = kInvalidNode;
    static constexpr uint32_t kNonSolidSides = 11;

    MixedScene() {
        root = makeElement(arena, kInvalidNode);
        arena.element(root).box.width = 400;
        arena.element(root).box.height = 300;
        square = addBox(arena, root, 10, 10, 100, 60, Edges{3, 3, 3, 3}, kAllDashed, 0, false);
        dotted = addBox(arena, root, 150, 20, 120, 90, Edges{2, 3, 4, 5}, kAllDotted, 12, true);
        mixed = addBox(arena, root, 40, 150, 200, 80, Edges{4, 2, 6, 3}, kMixed, 8, true);
    }
};

/// Paints `root` with `painter` into a fresh list and returns its bytes.
std::string paintBytes(Painter& painter, NodeArena& arena, NodeId root) {
    updateWorldTransforms(arena, root, /*force=*/true);
    CommandList list;
    painter.paint(arena, root, list);
    return listBytes(list);
}

}  // namespace

TEST_CASE("T-737 border dash cache: byte-identical on and off; the second paint hits every side") {
    MixedScene scene;
    Painter cached;
    Painter uncached;
    uncached.setBorderDashCacheEnabled(false);
    CHECK(cached.borderDashCacheEnabled());

    const std::string first = paintBytes(cached, scene.arena, scene.root);
    CHECK(first == paintBytes(uncached, scene.arena, scene.root));
    CHECK(cached.borderDashCacheStats().misses == MixedScene::kNonSolidSides);
    CHECK(cached.borderDashCacheStats().hits == 0);
    CHECK(cached.borderDashCacheStats().entries == MixedScene::kNonSolidSides);
    CHECK(cached.borderDashCacheStats().bytes > 0);
    CHECK(uncached.borderDashCacheStats().entries == 0);
    CHECK(uncached.borderDashCacheStats().misses == 0);

    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        const std::string bytes = paintBytes(cached, scene.arena, scene.root);
        CHECK(bytes == first);
        CHECK(bytes == paintBytes(uncached, scene.arena, scene.root));
        CHECK(cached.borderDashCacheStats().hits == MixedScene::kNonSolidSides);
        CHECK(cached.borderDashCacheStats().misses == 0);
        CHECK(cached.borderDashCacheStats().entries == MixedScene::kNonSolidSides);
    }
}

TEST_CASE("T-737 border dash cache: identical elements anywhere share one entry per side") {
    NodeArena arena;
    NodeId root = makeElement(arena, kInvalidNode);
    arena.element(root).box.width = 800;
    arena.element(root).box.height = 600;
    for (int i = 0; i < 15; ++i) {
        // 15 `.slot`s at different positions: the geometry is in each element's own frame.
        addBox(arena, root, 10.0f + static_cast<float>(i % 5) * 150.0f,
               10.0f + static_cast<float>(i / 5) * 120.0f, 120, 90, Edges{2, 2, 2, 2}, kAllDashed, 10, false);
    }
    Painter cached;
    Painter uncached;
    uncached.setBorderDashCacheEnabled(false);
    CHECK(paintBytes(cached, arena, root) == paintBytes(uncached, arena, root));
    // Four sides differ (side index); the 14 other slots hit the first one's entries.
    CHECK(cached.borderDashCacheStats().entries == 4);
    CHECK(cached.borderDashCacheStats().misses == 4);
    CHECK(cached.borderDashCacheStats().hits == 56);
    CHECK(paintBytes(cached, arena, root) == paintBytes(uncached, arena, root));
    CHECK(cached.borderDashCacheStats().hits == 60);
    CHECK(cached.borderDashCacheStats().misses == 0);
}

TEST_CASE("T-737 border dash cache: a changed width, radius or size misses and stays identical") {
    MixedScene scene;
    Painter cached;
    Painter uncached;
    uncached.setBorderDashCacheEnabled(false);
    (void)paintBytes(cached, scene.arena, scene.root);
    (void)paintBytes(cached, scene.arena, scene.root);
    REQUIRE(cached.borderDashCacheStats().misses == 0);

    SUBCASE("border width") {
        scene.arena.element(scene.square).box.border.top = 5;  // every corner of the box moves
    }
    SUBCASE("radius") {
        for (PropId p = prop::BorderRadiusTL; p <= prop::BorderRadiusBL; ++p) {
            scene.arena.element(scene.dotted).style.base[p] = Value::ofLength(20, LengthUnit::Px);
        }
    }
    SUBCASE("size") { scene.arena.element(scene.mixed).box.width = 210; }
    SUBCASE("scale (tolerance)") { scene.arena.common(scene.root).scale = Vec2{2, 2}; }
    const std::string bytes = paintBytes(cached, scene.arena, scene.root);
    CHECK(cached.borderDashCacheStats().misses > 0);
    CHECK(bytes == paintBytes(uncached, scene.arena, scene.root));
    // And the new geometry is served from the cache next time.
    CHECK(paintBytes(cached, scene.arena, scene.root) == bytes);
    CHECK(cached.borderDashCacheStats().misses == 0);
    CHECK(cached.borderDashCacheStats().hits == MixedScene::kNonSolidSides);
}

TEST_CASE("T-737 border dash cache: over the limits the least recently used entries go") {
    MixedScene scene;
    Painter uncached;
    uncached.setBorderDashCacheEnabled(false);
    const std::string expected = paintBytes(uncached, scene.arena, scene.root);

    SUBCASE("entries") {
        Painter cached;
        cached.setBorderDashCacheLimits(size_t{2} << 20, 3);
        for (int frame = 0; frame < 3; ++frame) {
            CHECK(paintBytes(cached, scene.arena, scene.root) == expected);
            CHECK(cached.borderDashCacheStats().entries == 3);
            CHECK(cached.borderDashCacheStats().hits + cached.borderDashCacheStats().misses ==
                  MixedScene::kNonSolidSides);
        }
    }
    SUBCASE("bytes") {
        Painter probe;
        (void)paintBytes(probe, scene.arena, scene.root);
        const uint32_t all = probe.borderDashCacheStats().bytes;
        REQUIRE(all > 0);
        Painter cached;
        cached.setBorderDashCacheLimits(all / 3, 256);
        for (int frame = 0; frame < 3; ++frame) {
            CHECK(paintBytes(cached, scene.arena, scene.root) == expected);
            CHECK(cached.borderDashCacheStats().bytes <= all / 3);
            CHECK(cached.borderDashCacheStats().entries > 0);
            CHECK(cached.borderDashCacheStats().entries < MixedScene::kNonSolidSides);
        }
    }
    SUBCASE("the default 256 entries") {
        NodeArena arena;
        NodeId root = makeElement(arena, kInvalidNode);
        arena.element(root).box.width = 2000;
        arena.element(root).box.height = 2000;
        for (int i = 0; i < 70; ++i) {  // 70 different heights x 4 sides = 280 keys
            addBox(arena, root, static_cast<float>(i % 10) * 150.0f, static_cast<float>(i / 10) * 150.0f, 100,
                   40.0f + static_cast<float>(i), Edges{2, 2, 2, 2}, kAllDashed, 0, false);
        }
        Painter cached;
        Painter plain;
        plain.setBorderDashCacheEnabled(false);
        CHECK(paintBytes(cached, arena, root) == paintBytes(plain, arena, root));
        CHECK(cached.borderDashCacheStats().entries == 256);
        CHECK(cached.borderDashCacheStats().misses == 280);
        CHECK(paintBytes(cached, arena, root) == paintBytes(plain, arena, root));
        CHECK(cached.borderDashCacheStats().entries == 256);
    }
}

TEST_CASE("T-737 border dash cache: entries unused for 120 paints are released") {
    MixedScene scene;
    NodeArena empty;
    NodeId emptyRoot = makeElement(empty, kInvalidNode);
    empty.element(emptyRoot).box.width = 10;
    empty.element(emptyRoot).box.height = 10;

    Painter cached;
    (void)paintBytes(cached, scene.arena, scene.root);
    REQUIRE(cached.borderDashCacheStats().entries == MixedScene::kNonSolidSides);
    for (int i = 0; i < 119; ++i) {
        (void)paintBytes(cached, empty, emptyRoot);
    }
    CHECK(cached.borderDashCacheStats().entries == MixedScene::kNonSolidSides);
    (void)paintBytes(cached, empty, emptyRoot);  // the 120th paint without them
    CHECK(cached.borderDashCacheStats().entries == 0);
    CHECK(cached.borderDashCacheStats().bytes == 0);

    // Used again before the limit: kept.
    (void)paintBytes(cached, scene.arena, scene.root);
    for (int i = 0; i < 100; ++i) {
        (void)paintBytes(cached, empty, emptyRoot);
    }
    (void)paintBytes(cached, scene.arena, scene.root);
    CHECK(cached.borderDashCacheStats().hits == MixedScene::kNonSolidSides);
    for (int i = 0; i < 100; ++i) {
        (void)paintBytes(cached, empty, emptyRoot);
    }
    CHECK(cached.borderDashCacheStats().entries == MixedScene::kNonSolidSides);
}

TEST_CASE("T-737 border dash cache: FrameStats carries the counters") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    Context ctx(ContextConfig{&shaper, &assets});
    compile::SourceFile files[1] = {compile::SourceFile{
        "main.xt",
        "<xt><head><style>.slot { width: 60px; height: 40px; margin: 4px; border: 2px #808080; border-style: "
        "dashed; }"
        "</style></head><body><div class=\"slot\"></div><div class=\"slot\"></div><div class=\"slot\"></div>"
        "</body></xt>"}};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(result.ok());
    REQUIRE(ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true).ok());
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.perf().setEnabled(true);
    CommandList list;
    DamageList damage;
    ctx.update(0.0f);
    ctx.render(list, &damage);
    ctx.update(0.0f);
    ctx.render(list, &damage);
    const FrameStats& stats = ctx.perf().history().back();
    CHECK(stats.borderDashHits == 12);  // three slots x four sides
    CHECK(stats.borderDashMisses == 0);
    CHECK(stats.borderDashEntries == 4);
    CHECK(stats.borderDashBytes > 0);
}

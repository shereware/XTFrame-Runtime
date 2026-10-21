#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "damage_internal.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/geom.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// DamageList / computeDamage: dirty rectangles from a command-list diff. See
/// docs/design/23-host-integration.md §23.2, docs/tasks/T-98.md "測試".

using namespace xt;

namespace {

constexpr float kViewW = 400;
constexpr float kViewH = 300;

/// A Context with one document compiled from an in-memory .xt string, plus the CommandList and
/// DamageList a host would keep across frames.
struct Harness {
    NullTextShaper shaper;
    FileAssetResolver assets;
    Context ctx;
    CommandList list;
    DamageList damage;
    DocumentHandle handle = 0;

    explicit Harness(const std::string& xtText)
        : assets(XT_TEST_LAYOUT_DIR, nullptr), ctx(ContextConfig{&shaper, &assets}) {
        compile::SourceFile files[1] = {compile::SourceFile{"main.xt", xtText}};
        compile::CompileOptions options;
        compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
        for (const compile::Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != compile::Diagnostic::Level::Error);
        }
        REQUIRE(result.ok());
        Result<DocumentHandle> loaded =
            ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setViewport(Rect{0, 0, kViewW, kViewH}, 1.0f);
    }

    /// One update + render, with damage tracking (the CommandList is deliberately not cleared:
    /// Context::render() takes the previous frame out of it).
    void frame(float dt = 0.0f) {
        ctx.update(dt);
        ctx.render(list, &damage);
    }

    Document& doc() {
        Document* d = ctx.document(handle);
        REQUIRE(d != nullptr);
        return *d;
    }

    void setStyle(const char* id, const char* declarations) {
        ElementRef ref = doc().byId(id);
        REQUIRE(ref.valid());
        CHECK(ref.setStyle(declarations) == Status::kOk);
    }
};

/// Bounding box of every damage rectangle.
Rect damageBounds(const DamageList& d) {
    Rect result{};
    for (const Rect& r : d.rects) {
        result = unite(result, r);
    }
    return result;
}

bool rectContains(const Rect& outer, const Rect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

std::string twoBoxes() {
    return "<xt><head><style>"
           "#a { position: absolute; left: 10px; top: 10px; width: 30px; height: 20px;"
           "     background-color: #ff0000; }"
           "#b { position: absolute; left: 300px; top: 240px; width: 30px; height: 20px;"
           "     background-color: #0000ff; }"
           "</style></head><body><div id=\"a\"></div><div id=\"b\"></div></body></xt>";
}

/// `count` 10x10 boxes spread over the viewport, ids "e0".."e<count-1>".
std::string manyBoxes(int count) {
    std::string style;
    std::string body;
    for (int i = 0; i < count; ++i) {
        std::string id = "e" + std::to_string(i);
        int left = 4 + (i % 5) * 70;
        int top = 4 + (i / 5) * 60;
        style += "#" + id + " { position: absolute; left: " + std::to_string(left) +
                 "px; top: " + std::to_string(top) +
                 "px; width: 10px; height: 10px; background-color: #ff0000; }";
        body += "<div id=\"" + id + "\"></div>";
    }
    return "<xt><head><style>" + style + "</style></head><body>" + body + "</body></xt>";
}

/// A 3x3 grid of 130x95 boxes covering almost the whole 400x300 viewport.
std::string gridBoxes() {
    std::string style;
    std::string body;
    for (int i = 0; i < 9; ++i) {
        std::string id = "g" + std::to_string(i);
        style += "#" + id + " { position: absolute; left: " + std::to_string((i % 3) * 133) +
                 "px; top: " + std::to_string((i / 3) * 100) +
                 "px; width: 130px; height: 95px; background-color: #ff0000; }";
        body += "<div id=\"" + id + "\"></div>";
    }
    return "<xt><head><style>" + style + "</style></head><body>" + body + "</body></xt>";
}

/// A perspective space root holding one 3D panel that fills it, placed at (`left`, `top`).
std::string perspectivePanel(int left, int top) {
    return "<xt><head><style>"
           "#stage { position: absolute; left: " +
           std::to_string(left) + "px; top: " + std::to_string(top) +
           "px; width: 80px; height: 60px; perspective: 400px; }"
           "#panel { position: absolute; left: 0px; top: 0px; width: 80px; height: 60px;"
           "         transform: rotateY(20deg); background-color: #3060c0; }"
           "</style></head><body><div id=\"stage\"><div id=\"panel\"></div></div></body></xt>";
}

/// Total area of every damage rectangle.
float damageArea(const DamageList& d) {
    float area = 0;
    for (const Rect& r : d.rects) {
        area += r.w * r.h;
    }
    return area;
}

}  // namespace

// §23.2 step 5 / T-98 test 1: the first frame has no previous list, so everything is dirty; an
// unchanged second frame reports no damage at all.
TEST_CASE("damage: first frame is full, an unchanged frame is empty") {
    Harness h(twoBoxes());
    h.frame();
    CHECK(h.damage.full);
    CHECK(h.damage.bounds() == Rect{0, 0, kViewW, kViewH});

    h.frame();
    CHECK(!h.damage.full);
    CHECK(h.damage.rects.empty());
    CHECK(h.damage.bounds() == Rect{0, 0, 0, 0});
}

// T-98 test 2: a moved absolute box damages exactly its old box union its new box, and nothing
// near the other element.
TEST_CASE("damage: a moved box damages its old and new bounds only") {
    Harness h(twoBoxes());
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());

    h.setStyle("a", "left: 50px;");
    h.frame(1.0f / 60.0f);

    CHECK(!h.damage.full);
    // The old (10,10,30,20) and the new (50,10,30,20) box: two disjoint rectangles, so they stay
    // separate (§23.2 step 4 only merges past kMaxDamageRects -- T-98's "只有一個矩形" would need
    // a merge rule the design does not have).
    CHECK(h.damage.rects.size() == 2);
    Rect bounds = damageBounds(h.damage);
    CHECK(std::fabs(bounds.x - 10.0f) <= 1.0f);
    CHECK(std::fabs(bounds.y - 10.0f) <= 1.0f);
    CHECK(std::fabs(bounds.w - 70.0f) <= 1.0f);
    CHECK(std::fabs(bounds.h - 20.0f) <= 1.0f);
    // #b sits at (300,240)-(330,260) and is untouched.
    for (const Rect& r : h.damage.rects) {
        CHECK(intersect(r, Rect{300, 240, 30, 20}).w == 0);
    }
}

// T-98 test 3: two far apart elements changing color give two rectangles, neither inside the
// other (and no duplicate of the unchanged geometry).
TEST_CASE("damage: two distant color changes give two independent rects") {
    Harness h(twoBoxes());
    h.frame();
    h.frame();

    h.setStyle("a", "background-color: #00ff00;");
    h.setStyle("b", "background-color: #00ff00;");
    h.frame(1.0f / 60.0f);

    CHECK(!h.damage.full);
    REQUIRE(h.damage.rects.size() == 2);
    CHECK(!rectContains(h.damage.rects[0], h.damage.rects[1]));
    CHECK(!rectContains(h.damage.rects[1], h.damage.rects[0]));
    CHECK(intersect(h.damage.rects[0], h.damage.rects[1]).w == 0);
}

// T-98 test 4 / §23.2 step 4: more changed commands than kMaxDamageRects merge down to 16.
TEST_CASE("damage: more than kMaxDamageRects changes merge down to 16") {
    Harness h(manyBoxes(20));
    h.frame();
    h.frame();

    for (int i = 0; i < 20; ++i) {
        h.setStyle(("e" + std::to_string(i)).c_str(), "background-color: #00ff00;");
    }
    h.frame(1.0f / 60.0f);

    CHECK(!h.damage.full);
    CHECK(h.damage.rects.size() <= kMaxDamageRects);
    CHECK(h.damage.rects.size() > 1);
}

// T-98 test 5 / §23.2 step 4: once the merged rectangles cover more than
// kFullDamageAreaRatio of the viewport, the frame is reported as full.
TEST_CASE("damage: changing most of the page reports full") {
    Harness h(gridBoxes());
    h.frame();
    h.frame();

    for (int i = 0; i < 7; ++i) {  // 7 of 9 boxes, ~68% of the viewport.
        h.setStyle(("g" + std::to_string(i)).c_str(), "background-color: #00ff00;");
    }
    h.frame(1.0f / 60.0f);

    CHECK(h.damage.full);
    CHECK(h.damage.rects.empty());
    CHECK(h.damage.bounds() == Rect{0, 0, kViewW, kViewH});
}

// T-98 test 6 / §23.2 step 5: a viewport change invalidates the previous frame.
TEST_CASE("damage: setViewport forces the next frame to be full") {
    Harness h(twoBoxes());
    h.frame();
    h.frame();
    REQUIRE(!h.damage.full);

    h.ctx.setViewport(Rect{0, 0, 320, 200}, 1.0f);
    h.frame(1.0f / 60.0f);
    CHECK(h.damage.full);
    CHECK(h.damage.bounds() == Rect{0, 0, 320, 200});
}

// T-98 test 7: an effect's damage is outset by its expansion. blur(8px) is radius 8 ->
// sigma = 4 -> 3 sigma = 12 px per side (effectExpand(), docs/design/18-effects.md §18.3); the
// task sheet's "24 px" assumed sigma == radius, which the core formula does not use.
TEST_CASE("damage: a blurred element's damage is outset by the effect expansion") {
    Harness h(
        "<xt><head><style>"
        "#fx { position: absolute; left: 100px; top: 100px; width: 40px; height: 40px;"
        "      background-color: #ff0000; effect: blur(8px); }"
        "</style></head><body><div id=\"fx\"></div></body></xt>");
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());

    h.setStyle("fx", "left: 200px;");
    h.frame(1.0f / 60.0f);

    REQUIRE(!h.damage.full);
    REQUIRE(!h.damage.rects.empty());
    const float kExpand = 12.0f;  // 3 * (8 / 2)
    Rect bounds = damageBounds(h.damage);
    CHECK(bounds.x <= 100.0f - kExpand);
    CHECK(bounds.y <= 100.0f - kExpand);
    CHECK(bounds.x + bounds.w >= 240.0f + kExpand);
    CHECK(bounds.y + bounds.h >= 140.0f + kExpand);
    // Both the old and the new position are covered.
    bool coversOld = false;
    bool coversNew = false;
    for (const Rect& r : h.damage.rects) {
        coversOld = coversOld || rectContains(r, Rect{100, 100, 40, 40});
        coversNew = coversNew || rectContains(r, Rect{200, 100, 40, 40});
    }
    CHECK(coversOld);
    CHECK(coversNew);
}

// T-130 (docs/design/18-effects.md §18.6): a BackdropBlur node reads what lies below it, so a
// change underneath it dirties the panel even though none of the panel's own commands moved.
// computeDamage unions the panel's own rectangle (plus the blur's 3 sigma read range) with any
// dirty rectangle it meets.
TEST_CASE("damage: a backdrop-blur panel joins the damage when something under it moves") {
    Harness h(
        "<xt><head><style>"
        "#b  { position: absolute; left: 20px; top: 140px; width: 40px; height: 20px;"
        "      background-color: #ff0000; }"
        "#fx { position: absolute; left: 100px; top: 100px; width: 60px; height: 60px;"
        "      background-color: #ffffff80; effect: backdrop-blur(8px); }"
        "</style></head><body><div id=\"b\"></div><div id=\"fx\"></div></body></xt>");
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());

    // Far from the panel: only the box itself is dirty.
    h.setStyle("b", "left: 24px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(!h.damage.full);
    bool coversPanelWhenFar = false;
    for (const Rect& r : h.damage.rects) {
        coversPanelWhenFar = coversPanelWhenFar || rectContains(r, Rect{100, 100, 60, 60});
    }
    CHECK(coversPanelWhenFar == false);

    // Moved under the panel: the panel's own rectangle has to be repainted too.
    h.setStyle("b", "left: 110px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(!h.damage.full);
    bool coversPanel = false;
    for (const Rect& r : h.damage.rects) {
        coversPanel = coversPanel || rectContains(r, Rect{100, 100, 60, 60});
    }
    CHECK(coversPanel);
}

// T-105 test 1 (docs/design/19-3d-space.md §19.6 step 1): a small 3D panel rotating in a corner
// damages its own projected extent, not the whole viewport -- before ProjectionCmd::boundsHint,
// PushProjection reported the viewport and every such frame was 100% dirty.
TEST_CASE("damage: a rotating 3D panel damages its projected subtree, not the viewport") {
    Harness h(perspectivePanel(10, 10));
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());

    h.setStyle("panel", "transform: rotateY(50deg);");
    h.frame(1.0f / 60.0f);

    REQUIRE(!h.damage.full);
    REQUIRE(!h.damage.rects.empty());
    CHECK(damageArea(h.damage) < 0.25f * kViewW * kViewH);
    // The panel sits in the top-left corner; nothing near the opposite corner is dirty.
    for (const Rect& r : h.damage.rects) {
        CHECK(intersect(r, Rect{300, 240, 100, 60}).w == 0);
    }
}

// T-105 test 2: the hint is clipped to the viewport, so a space root half off-screen damages only
// the on-screen half.
TEST_CASE("damage: a half-offscreen 3D panel's damage is clipped to the viewport") {
    Harness h(perspectivePanel(360, 120));
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());

    h.setStyle("panel", "transform: rotateY(50deg);");
    h.frame(1.0f / 60.0f);

    REQUIRE(!h.damage.full);
    REQUIRE(!h.damage.rects.empty());
    Rect bounds = damageBounds(h.damage);
    CHECK(bounds.x >= 0.0f);
    CHECK(bounds.y >= 0.0f);
    CHECK(bounds.x + bounds.w <= kViewW);
    CHECK(bounds.y + bounds.h <= kViewH);
    CHECK(bounds.x >= 300.0f);  // Nothing left of the stage is dirty.
}

// T-98 test 8: the per-command hashes of two identical frames match entry by entry (the property
// the whole diff rests on).
TEST_CASE("damage: command hashes are stable across identical frames") {
    Harness h(twoBoxes());
    h.frame();
    CommandList first = h.list;
    h.frame();

    std::vector<uint64_t> a;
    std::vector<uint64_t> b;
    hashCommandList(first, a);
    hashCommandList(h.list, b);
    REQUIRE(a.size() == b.size());
    REQUIRE(!a.empty());
    for (size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i] == b[i]);
    }
}

TEST_CASE("damage: DrawImageMesh bounds are its vertices' AABB and a moved vertex changes its hash") {
    CommandList list;
    ImageMeshCmd mc{};
    mc.asset = 0;
    mc.vertexOffset = 1;  // the first pool entry belongs to nobody
    mc.vertexCount = 3;
    mc.indexOffset = 0;
    mc.indexCount = 3;
    mc.opacity = 1;
    list.meshVertices = {MeshVertex{{-500, -500}, {0, 0}}, MeshVertex{{12, 40}, {0, 0}},
                         MeshVertex{{70, 15}, {1, 0}}, MeshVertex{{30, 90}, {0, 1}}};
    list.meshIndices = {0, 1, 2};
    list.imageMeshes.push_back(mc);
    list.commands.push_back(Command{CmdKind::DrawImageMesh, {}, 0});

    const Rect box = commandBounds(list, list.commands[0]);
    CHECK(box.x == doctest::Approx(12));
    CHECK(box.y == doctest::Approx(15));
    CHECK(box.w == doctest::Approx(58));
    CHECK(box.h == doctest::Approx(75));

    std::vector<uint64_t> before;
    hashCommandList(list, before);
    list.meshVertices[2].p.x = 71;
    std::vector<uint64_t> after;
    hashCommandList(list, after);
    REQUIRE(before.size() == 1);
    REQUIRE(after.size() == 1);
    CHECK(before[0] != after[0]);
    list.meshVertices[2].p.x = 70;
    list.meshIndices[0] = 2;
    hashCommandList(list, after);
    CHECK(before[0] != after[0]);
}

namespace {

/// Appends a closed polygon path through `pts` (x, y pairs) to `list`'s pools.
PathRef addPolygon(CommandList& list, std::initializer_list<float> pts) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(list.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(list.points.size());
    const size_t n = pts.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        list.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
    }
    list.verbs.push_back(PathVerb::Close);
    list.points.insert(list.points.end(), pts.begin(), pts.end());
    ref.verbCount = static_cast<uint32_t>(n + 1);
    ref.pointCount = static_cast<uint32_t>(pts.size());
    return ref;
}

void addFill(CommandList& list, const PathRef& path, Color color) {
    FillCmd f{};
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    list.fills.push_back(f);
    list.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(list.fills.size() - 1)});
}

/// The bounds of what strokeToFill actually draws for `c` (the reference the damage must cover).
Rect strokedGeometryBounds(const CommandList& list, const StrokeCmd& c) {
    std::vector<Vec2> flat;
    std::vector<uint32_t> starts;
    std::vector<uint8_t> closed;
    flattenPath(std::span<const PathVerb>(list.verbs.data() + c.path.verbOffset, c.path.verbCount),
                std::span<const float>(list.points.data() + c.path.pointOffset, c.path.pointCount), c.xf,
                kFlattenTolerance, flat, starts, closed);
    std::vector<Vec2> outPts;
    std::vector<uint32_t> outStarts;
    strokeToFill(flat, starts, closed, c.width * std::sqrt(std::fabs(c.xf.a * c.xf.d - c.xf.b * c.xf.c)),
                 c.cap, c.join, c.miterLimit, kFlattenTolerance, outPts, outStarts);
    REQUIRE(!outPts.empty());
    float x0 = outPts[0].x, y0 = outPts[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : outPts) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

// T-263a B6: a glow whose content changes colour keeps its PushEffect in the common prefix; the
// damage must still cover the halo (the effect's boundsHint), not just the fill's own box.
TEST_CASE("damage: T-263a content changing inside an effect damages the effect's halo") {
    auto build = [](Color color) {
        CommandList list;
        list.viewport = Rect{0, 0, kViewW, kViewH};
        list.devicePixelRatio = 1;
        EffectInstance glow{};
        glow.kind = EffectKind::Glow;
        glow.params[0] = 16;
        list.effects.push_back(glow);
        EffectCmd ec{};
        ec.effectOffset = 0;
        ec.effectCount = 1;
        ec.boundsHint = Rect{76, 76, 88, 88};  // the 40 x 40 box outset by the glow's reach
        ec.contentBounds = Rect{100, 100, 40, 40};
        list.effectCmds.push_back(ec);
        addFill(list, addPolygon(list, {0, 0, 10, 0, 10, 10}), Color{0, 0, 0, 1});  // unchanged, before
        list.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
        addFill(list, addPolygon(list, {100, 100, 140, 100, 140, 140, 100, 140}), color);
        list.commands.push_back(Command{CmdKind::PopEffect, {}, 0});
        addFill(list, addPolygon(list, {300, 200, 310, 200, 310, 210}),
                Color{0, 0, 0, 1});  // unchanged, after
        return list;
    };
    const CommandList prev = build(Color{1, 0, 0, 1});
    const CommandList cur = build(Color{0, 1, 0, 1});
    DamageList damage;
    computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, damage);
    REQUIRE(!damage.full);
    REQUIRE(!damage.rects.empty());
    bool coversHalo = false;
    for (const Rect& r : damage.rects) {
        coversHalo = coversHalo || rectContains(r, Rect{76, 76, 88, 88});
        CHECK(intersect(r, Rect{300, 200, 10, 10}).w == 0);
    }
    CHECK(coversHalo);
}

// T-263a B7: a stroke's damage covers its miter tips and square caps, i.e. everything
// strokeToFill draws, not just width / 2 around the path.
TEST_CASE("damage: T-263a stroke bounds cover miter spikes and square caps") {
    CommandList list;
    // A five-pointed star (sharp 36-degree spikes) around (100, 100).
    std::vector<float> star;
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 5.0f - 3.14159265f / 2.0f;
        const float r = (i % 2 == 0) ? 50.0f : 20.0f;
        star.push_back(100.0f + r * std::cos(a));
        star.push_back(100.0f + r * std::sin(a));
    }
    PathRef starPath{};
    starPath.verbOffset = 0;
    starPath.pointOffset = 0;
    for (int i = 0; i < 10; ++i) {
        list.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
    }
    list.verbs.push_back(PathVerb::Close);
    list.points = star;
    starPath.verbCount = 11;
    starPath.pointCount = 20;
    // An open horizontal line for the cap case.
    PathRef line{};
    line.verbOffset = static_cast<uint32_t>(list.verbs.size());
    line.pointOffset = static_cast<uint32_t>(list.points.size());
    list.verbs.push_back(PathVerb::Move);
    list.verbs.push_back(PathVerb::Line);
    const float linePts[4] = {200, 50, 260, 80};
    list.points.insert(list.points.end(), linePts, linePts + 4);
    line.verbCount = 2;
    line.pointCount = 4;

    struct Case {
        const char* name;
        PathRef path;
        LineJoin join;
        LineCap cap;
        float miterLimit;
        Mat2x3 xf;
    };
    const Case cases[] = {
        {"miter star", starPath, LineJoin::Miter, LineCap::Butt, 4.0f, Mat2x3{}},
        {"miter star, scaled", starPath, LineJoin::Miter, LineCap::Butt, 4.0f, scaling(Vec2{2, 2})},
        {"square cap, low miter limit", line, LineJoin::Miter, LineCap::Square, 1.0f, Mat2x3{}},
        {"square cap, bevel", line, LineJoin::Bevel, LineCap::Square, 4.0f, Mat2x3{}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        StrokeCmd sc{};
        sc.xf = c.xf;
        sc.path = c.path;
        sc.paint.kind = PaintKind::Solid;
        sc.paint.color = Color{0, 0, 0, 1};
        sc.width = 10;
        sc.cap = c.cap;
        sc.join = c.join;
        sc.miterLimit = c.miterLimit;
        list.strokes.clear();
        list.strokes.push_back(sc);
        const Command cmd{CmdKind::StrokePath, {}, 0};
        const Rect damage = commandBounds(list, cmd);
        const Rect drawn = strokedGeometryBounds(list, sc);
        CHECK(damage.x <= drawn.x + 1e-3f);
        CHECK(damage.y <= drawn.y + 1e-3f);
        CHECK(damage.x + damage.w >= drawn.x + drawn.w - 1e-3f);
        CHECK(damage.y + damage.h >= drawn.y + drawn.h - 1e-3f);
    }
    // The shared reach: miter limit, sqrt(2) for a square cap, one half-width otherwise.
    CHECK(strokeOutset(10, LineCap::Butt, LineJoin::Miter, 4) == doctest::Approx(20));
    CHECK(strokeOutset(10, LineCap::Square, LineJoin::Bevel, 4) == doctest::Approx(5 * std::sqrt(2.0f)));
    CHECK(strokeOutset(10, LineCap::Round, LineJoin::Round, 4) == doctest::Approx(5));
    CHECK(strokeOutset(10, LineCap::Butt, LineJoin::Miter, 0.5f) == doctest::Approx(5));
}

// T-263b: B6's shape of problem (a change inside a section whose Push stays in the common prefix)
// checked for the other enclosing sections, against the pixels the software renderer draws: a
// blended layer and a mask combine per pixel, so the changed commands' own bounds must do.
namespace {

std::vector<uint8_t> renderFull(const CommandList& list) {
    const int w = static_cast<int>(kViewW);
    const int h = static_cast<int>(kViewH);
    std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{px.data(), w, h, w * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    return px;
}

struct PixelDamage {
    int changed = 0;  ///< pixels that differ between the two full renders
    int outside = 0;  ///< of those, the ones no damage rectangle contains
    bool full = false;
};

PixelDamage pixelDamage(const CommandList& prev, const CommandList& cur) {
    REQUIRE(validateCommandList(prev));
    REQUIRE(validateCommandList(cur));
    DamageList damage;
    computeDamage(prev, cur, Rect{0, 0, kViewW, kViewH}, damage);
    const std::vector<uint8_t> a = renderFull(prev);
    const std::vector<uint8_t> b = renderFull(cur);
    PixelDamage result;
    result.full = damage.full;
    const int w = static_cast<int>(kViewW);
    const int h = static_cast<int>(kViewH);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            if (a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2] && a[i + 3] == b[i + 3]) {
                continue;
            }
            ++result.changed;
            bool inside = damage.full;
            for (const Rect& r : damage.rects) {
                inside = inside || (static_cast<float>(x) >= r.x && static_cast<float>(x + 1) <= r.x + r.w &&
                                    static_cast<float>(y) >= r.y && static_cast<float>(y + 1) <= r.y + r.h);
            }
            result.outside += inside ? 0 : 1;
        }
    }
    return result;
}

void addBox(CommandList& list, float x, float y, float w, float h, Color color) {
    addFill(list, addPolygon(list, {x, y, x + w, y, x + w, y + h, x, y + h}), color);
}

CommandList emptyList() {
    CommandList list;
    list.viewport = Rect{0, 0, kViewW, kViewH};
    list.devicePixelRatio = 1;
    return list;
}

/// A backdrop, then a layer (large unchanged content plus one small box at `boxX` in `boxColor`),
/// then an unchanged box: PushLayer stays in the common prefix when only the small box changes.
CommandList layerList(BlendMode blend, float boxX, Color boxColor) {
    CommandList list = emptyList();
    addBox(list, 20, 20, 360, 260, Color{0.6f, 0.7f, 0.8f, 1});
    LayerCmd layer{};
    layer.opacity = 0.8f;
    layer.blend = blend;
    layer.boundsHint = Rect{60, 60, 200, 160};
    list.layers.push_back(layer);
    list.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    addBox(list, 60, 60, 200, 160, Color{0.9f, 0.6f, 0.3f, 1});
    addBox(list, boxX, 100, 40, 40, boxColor);
    list.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    addBox(list, 300, 200, 20, 20, Color{0, 0, 0, 1});
    return list;
}

/// A backdrop, then a mask whose shape is a box at `shapeX` (plus an unchanged half-transparent
/// one) over content of `contentColor`, then an unchanged box.
CommandList maskList(MaskMode mode, float shapeX, Color contentColor) {
    CommandList list = emptyList();
    addBox(list, 20, 20, 360, 260, Color{0.6f, 0.7f, 0.8f, 1});
    MaskCmd mask{};
    mask.mode = mode;
    mask.boundsHint = Rect{60, 60, 200, 160};
    list.masks.push_back(mask);
    list.commands.push_back(Command{CmdKind::PushMask, {}, 0});
    addBox(list, 200, 90, 40, 100, Color{0.5f, 0.5f, 0.5f, 0.5f});
    addBox(list, shapeX, 80, 80, 80, Color{1, 1, 1, 1});
    list.commands.push_back(Command{CmdKind::EndMaskContent, {}, 0});
    addBox(list, 60, 60, 200, 160, Color{0.2f, 0.4f, 0.9f, 1});
    addBox(list, 90, 100, 120, 60, contentColor);
    list.commands.push_back(Command{CmdKind::PopMask, {}, 0});
    addBox(list, 300, 200, 20, 20, Color{0, 0, 0, 1});
    return list;
}

}  // namespace

TEST_CASE("damage: T-263b a change inside a blended layer damages every pixel that changes") {
    for (BlendMode blend : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay,
                            BlendMode::ColorDodge, BlendMode::Difference}) {
        CAPTURE(static_cast<int>(blend));
        // The content changes colour, then moves.
        const PixelDamage recolored =
            pixelDamage(layerList(blend, 100, Color{1, 0, 0, 1}), layerList(blend, 100, Color{0, 1, 0, 1}));
        CHECK(recolored.changed > 1000);
        CHECK(recolored.outside == 0);
        CHECK(!recolored.full);
        const PixelDamage moved =
            pixelDamage(layerList(blend, 100, Color{1, 0, 0, 1}), layerList(blend, 180, Color{1, 0, 0, 1}));
        CHECK(moved.changed > 1000);
        CHECK(moved.outside == 0);
        CHECK(!moved.full);
    }
}

TEST_CASE("damage: T-263b a change inside a mask damages every pixel that changes") {
    for (MaskMode mode : {MaskMode::Alpha, MaskMode::AlphaInverted, MaskMode::Luma, MaskMode::LumaInverted}) {
        CAPTURE(static_cast<int>(mode));
        // The masked content changes colour, then the mask shape moves.
        const PixelDamage recolored =
            pixelDamage(maskList(mode, 80, Color{1, 0, 0, 1}), maskList(mode, 80, Color{0, 1, 0, 1}));
        CHECK(recolored.changed > 100);
        CHECK(recolored.outside == 0);
        CHECK(!recolored.full);
        const PixelDamage moved =
            pixelDamage(maskList(mode, 80, Color{1, 0, 0, 1}), maskList(mode, 120, Color{1, 0, 0, 1}));
        CHECK(moved.changed > 100);
        CHECK(moved.outside == 0);
        CHECK(!moved.full);
    }
}

// T-263c ruling 2: `out` comes back from the host, which may have edited it or handed over another
// list. Only the list this Context produced in its previous render() is a base to diff against;
// anything else is "no base": the whole viewport is damaged and the list is never indexed.
TEST_CASE("damage: T-263c only the list this Context rendered last is a damage base") {
    Harness h(twoBoxes());
    h.frame();
    h.frame();
    REQUIRE(!h.damage.full);
    REQUIRE(h.damage.rects.empty());

    SUBCASE("another Context's list with the same commands") {
        Harness other(twoBoxes());
        other.frame();
        std::swap(h.list, other.list);
    }
    SUBCASE("a list built by hand whose command indexes past its pools") {
        CommandList broken;
        broken.viewport = h.list.viewport;
        broken.devicePixelRatio = h.list.devicePixelRatio;
        broken.commands.push_back(Command{CmdKind::FillPath, {}, 7});  // there is no fills[7]
        broken.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0xFFFFFFFFu});
        h.list = broken;
    }
    SUBCASE("a list the host cleared") { h.list.clear(); }
    SUBCASE("a list marked as not checked (edited by hand)") { h.list.structureChecked = false; }
    SUBCASE("an older list of this Context") {
        const CommandList older = h.list;
        h.setStyle("a", "left: 100px");
        h.frame();
        CHECK(!h.damage.full);
        h.list = older;  // What is on screen is the frame just rendered, not this one.
    }
    h.frame();
    CHECK(h.damage.full);
    CHECK(h.list.structureChecked);

    // The frame after that is diffed against this Context's own list again.
    h.frame();
    CHECK(!h.damage.full);
    CHECK(h.damage.rects.empty());
}

// T-263e ruling C: a list simplified in place is not a damage base (T-263c, the correct
// behaviour), so a host that simplifies got "full" every frame. The out-of-place form leaves the
// list Context::render() wrote untouched: it stays the base and the damage stays partial.
TEST_CASE("damage: T-263e a list simplified out of place leaves the damage base alone") {
    // What a host that does not simplify gets for the same two frames.
    Harness reference(twoBoxes());
    reference.frame();
    reference.setStyle("a", "left: 50px;");
    reference.frame(1.0f / 60.0f);
    REQUIRE(!reference.damage.full);
    REQUIRE(reference.damage.rects.size() == 2);

    Harness h(twoBoxes());
    CommandListSimplifier simplifier;
    const SimplifyOptions options;
    CommandList simplified;
    h.frame();
    CHECK(h.damage.full);  // the first frame has no base

    SUBCASE("out of place: the second frame's damage is partial and equals the non-simplified run's") {
        simplifier.simplify(h.list, simplified, options, &h.shaper);
        CHECK(simplified.structureChecked);
        CHECK(simplified.frameSerial == 0);
        CHECK(h.list.frameSerial != 0);
        h.setStyle("a", "left: 50px;");
        h.frame(1.0f / 60.0f);
        simplifier.simplify(h.list, simplified, options, &h.shaper);

        CHECK(!h.damage.full);
        REQUIRE(h.damage.rects.size() == reference.damage.rects.size());
        for (size_t i = 0; i < reference.damage.rects.size(); ++i) {
            CHECK(h.damage.rects[i] == reference.damage.rects[i]);
        }
        // And an unchanged third frame reports nothing, as without the simplifier.
        h.frame();
        CHECK(!h.damage.full);
        CHECK(h.damage.rects.empty());
    }
    SUBCASE("in place: the rewritten list is no base, the next frame is full (T-263c)") {
        simplifier.simplify(h.list, options, &h.shaper);
        CHECK(h.list.frameSerial == 0);
        h.setStyle("a", "left: 50px;");
        h.frame(1.0f / 60.0f);
        CHECK(h.damage.full);
    }
}

// T-326 (18 §18.12, the §18.3 BackdropBlur rule applied to blended Layers): an element with a
// non-Normal `mix-blend-mode` mixes with what lies below it, so a change underneath repaints the
// element too; a change of its own blend mode repaints it; nothing below it is dragged in.
TEST_CASE("damage: T-326 a blended element joins the damage when something under it moves") {
    Harness h(
        "<xt><head><style>"
        "#b  { position: absolute; left: 20px; top: 140px; width: 40px; height: 20px;"
        "      background-color: #ff0000; }"
        "#fx { position: absolute; left: 100px; top: 100px; width: 60px; height: 60px;"
        "      background-color: #ff8000; mix-blend-mode: multiply; }"
        "</style></head><body><div id=\"b\"></div><div id=\"fx\"></div></body></xt>");
    h.frame();
    h.frame();
    REQUIRE(h.damage.rects.empty());
    // The style reached the node: the frame holds one multiply Layer.
    bool multiplyLayer = false;
    for (const LayerCmd& layer : h.list.layers) {
        multiplyLayer = multiplyLayer || layer.blend == BlendMode::Multiply;
    }
    CHECK(multiplyLayer);

    // Far from the element: only the box itself is dirty.
    h.setStyle("b", "left: 24px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(!h.damage.full);
    bool coversWhenFar = false;
    for (const Rect& r : h.damage.rects) {
        coversWhenFar = coversWhenFar || rectContains(r, Rect{100, 100, 60, 60});
    }
    CHECK(coversWhenFar == false);

    // Moved under the element: the element's own rectangle has to be repainted too.
    h.setStyle("b", "left: 110px;");
    h.frame(1.0f / 60.0f);
    REQUIRE(!h.damage.full);
    bool covers = false;
    for (const Rect& r : h.damage.rects) {
        covers = covers || rectContains(r, Rect{100, 100, 60, 60});
    }
    CHECK(covers);

    // A new blend mode on the element itself repaints the element, not the box far away.
    h.setStyle("b", "left: 20px;");
    h.frame(1.0f / 60.0f);
    h.frame(1.0f / 60.0f);
    REQUIRE(h.damage.rects.empty());
    h.setStyle("fx", "mix-blend-mode: screen;");
    h.frame(1.0f / 60.0f);
    REQUIRE(!h.damage.full);
    bool coversSelf = false;
    bool coversBox = false;
    for (const Rect& r : h.damage.rects) {
        coversSelf = coversSelf || rectContains(r, Rect{100, 100, 60, 60});
        const Rect hit = intersect(r, Rect{20, 140, 40, 20});
        coversBox = coversBox || (hit.w > 0 && hit.h > 0);
    }
    CHECK(coversSelf);
    CHECK(coversBox == false);
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/timeline.h"

/// T-228 (Docs/design/31-vector-authoring.md §31.7.2): `auto-orient: path` adds the tangent angle
/// of the node's position track to its rotation. See Docs/tasks/T-228.md item 1.

using namespace xt;
using namespace xt::compile;

namespace {

Keyframe key(float time, Vec2 v) {
    Keyframe k;
    k.time = time;
    k.value = Value::ofVec2(v);
    k.easing = Easing::linear();
    return k;
}

/// A quarter circle of radius 100 around the origin, (100, 0) -> (0, 100), as one spatial-bezier
/// segment (handle length 100 * 0.5523), then a hold at (0, 100) until t = 2.
Timeline makeArc() {
    Timeline tl;
    tl.duration = 2.0f;
    Keyframe k0 = key(0.0f, Vec2{100, 0});
    k0.hasSpatial = 1;
    k0.tanOut = Vec3{0, 55.23f, 0};
    k0.tanIn = Vec3{55.23f, 0, 0};
    tl.keyframes.push_back(k0);
    tl.keyframes.push_back(key(1.0f, Vec2{0, 100}));
    tl.keyframes.push_back(key(2.0f, Vec2{0, 100}));
    Track t;
    t.target = 0;
    t.prop = prop::Translate;
    t.type = ValueType::Vec2;
    t.kfOffset = 0;
    t.kfCount = 3;
    tl.tracks.push_back(t);
    return tl;
}

float normDeg(float d) {
    while (d > 180.0f) {
        d -= 360.0f;
    }
    while (d <= -180.0f) {
        d += 360.0f;
    }
    return d;
}

CompileResult compileMain(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

struct Loaded {
    CompileResult result;  // Document::load keeps pointing into the IR blob.
    std::unique_ptr<Document> doc;
};

Loaded load(const std::string& xt) {
    Loaded out;
    out.result = compileMain(xt);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 400});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

TEST_CASE("auto-orient: PropId 14 metadata") {
    CHECK(propertyDefined(prop::AutoOrient));
    CHECK(propertyType(prop::AutoOrient) == ValueType::Enum);
    CHECK(propertyApplies(NodeKind::Group, prop::AutoOrient));
    CHECK(propertyApplies(NodeKind::Path, prop::AutoOrient));
    CHECK(propertyApplies(NodeKind::Element, prop::AutoOrient));
    CHECK_FALSE(propertyApplies(NodeKind::Free, prop::AutoOrient));
    CHECK(propertyByName("AutoOrient") == prop::AutoOrient);
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);
    setProperty(arena, n, prop::AutoOrient, Value::ofEnum(1));
    CHECK(getProperty(arena, n, prop::AutoOrient).e == 1u);
}

TEST_CASE("auto-orient: rotation follows the tangent of a circular spatial-bezier path") {
    Timeline tl = makeArc();
    NodeArena arena;
    NodeId node = arena.create(NodeKind::Group);
    NodeId plain = arena.create(NodeKind::Group);
    setProperty(arena, node, prop::Rotation, Value::ofFloat(10.0f));
    setProperty(arena, node, prop::AutoOrient, Value::ofEnum(1));
    NodeId map[1] = {node};
    NodeId mapPlain[1] = {plain};

    Animator anim;
    TimelineId tid = anim.registerTimeline(&tl);
    PlayParams p;
    p.timeline = tid;
    p.localToArena = map;
    p.localCount = 1;
    p.selfNode = kInvalidNode;
    anim.play(p, 0.0f);
    PlayParams q = p;
    q.localToArena = mapPlain;
    anim.play(q, 0.0f);

    anim.evaluate(0.0f, arena);
    CHECK(normDeg(arena.common(node).orientPathDeg) == doctest::Approx(90.0f).epsilon(0.01));
    anim.evaluate(0.5f, arena);
    CHECK(normDeg(arena.common(node).orientPathDeg) == doctest::Approx(135.0f).epsilon(0.01));
    // The authored rotation is untouched; the tangent angle is added when composing.
    CHECK(getProperty(arena, node, prop::Rotation).f == doctest::Approx(10.0f));
    anim.evaluate(0.999f, arena);
    CHECK(normDeg(arena.common(node).orientPathDeg) == doctest::Approx(180.0f).epsilon(0.02));
    const float atEnd = arena.common(node).orientPathDeg;
    // Stationary (1 s -> 2 s holds the position): the last angle is kept.
    anim.evaluate(1.5f, arena);
    CHECK(arena.common(node).orientPathDeg == doctest::Approx(atEnd));
    // Without auto-orient nothing is added.
    CHECK(arena.common(plain).orientPathDeg == 0.0f);

    // The composed transform rotates by rotation + tangent (10 + ~180 degrees).
    updateWorldTransforms(arena, node, true);
    const Mat2x3& m = arena.common(node).local;
    const float composed = std::atan2(m.b, m.a) * 180.0f / 3.14159265f;
    CHECK(normDeg(composed) == doctest::Approx(normDeg(10.0f + atEnd)).epsilon(0.01));

    // Switching auto-orient off drops the added angle.
    setProperty(arena, node, prop::AutoOrient, Value::ofEnum(0));
    CHECK(arena.common(node).orientPathDeg == 0.0f);
}

TEST_CASE("auto-orient: straight segments use the chord; single-axis tracks too") {
    Timeline tl;
    tl.duration = 2.0f;
    tl.keyframes.push_back(key(0.0f, Vec2{0, 0}));
    tl.keyframes.push_back(key(1.0f, Vec2{100, 0}));
    tl.keyframes.push_back(key(2.0f, Vec2{100, 100}));
    Track t;
    t.target = 0;
    t.prop = prop::Translate;
    t.type = ValueType::Vec2;
    t.kfOffset = 0;
    t.kfCount = 3;
    tl.tracks.push_back(t);
    Vec2 d{};
    REQUIRE(evalTrackDirection(tl, tl.tracks[0], 0.5f, d));
    CHECK(d.x > 0.0f);
    CHECK(d.y == doctest::Approx(0.0f));
    REQUIRE(evalTrackDirection(tl, tl.tracks[0], 1.5f, d));
    CHECK(d.x == doctest::Approx(0.0f));
    CHECK(d.y > 0.0f);
}

TEST_CASE("auto-orient: `auto-orient: path` on an element orients along its transform keys") {
    Loaded l = load(R"(<xt><head><style>
      #Car { position: absolute; left: 0px; top: 0px; width: 20px; height: 10px; auto-orient: path; }
      #Still { position: absolute; left: 0px; top: 0px; width: 20px; height: 10px; }
      @timeline main {
        #Car { 0s { transform: translate(0px, 0px) } 1s { transform: translate(100px, 0px) }
               2s { transform: translate(100px, 100px) } }
        #Still { 0s { transform: translate(0px, 0px) } 1s { transform: translate(100px, 0px) } }
      }
    </style></head><body><div id="Car"></div><div id="Still"></div></body></xt>)");
    Document& d = *l.doc;
    ElementRef car = d.byId("Car");
    ElementRef still = d.byId("Still");
    REQUIRE(car.valid());
    CHECK(d.arena().common(car.node).autoOrient == 1);
    CHECK(d.arena().common(still.node).autoOrient == 0);
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(normDeg(d.arena().common(car.node).orientPathDeg) == doctest::Approx(0.0f));
    REQUIRE(d.seekTimeline("", "main", 1.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(normDeg(d.arena().common(car.node).orientPathDeg) == doctest::Approx(90.0f));
    CHECK(d.arena().common(still.node).orientPathDeg == 0.0f);
}

TEST_CASE("motion-in / motion-out: @timeline translate keys become a spatial Vec2 track") {
    Loaded l = load(R"(<xt><head><style>
      #Car { position: absolute; left: 0px; top: 0px; width: 20px; height: 10px; auto-orient: path; }
      @timeline main {
        #Car { 0s { transform: translate(0px, 0px); motion-out: 0px 80px }
               1s { transform: translate(100px, 0px); motion-in: 0px 80px } }
      }
    </style></head><body><div id="Car"></div>
      <svg id="Icon" viewBox="0 0 100 100" width="100" height="100"><path id="p" d="M0 0 L10 0 L10 10 Z"/></svg>
    </body></xt>)");
    Document& d = *l.doc;
    ElementRef car = d.byId("Car");
    REQUIRE(car.valid());
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    const Vec2 mid = d.arena().element(car.node).cssTranslate;
    CHECK(mid.x == doctest::Approx(50.0f));
    CHECK(mid.y == doctest::Approx(60.0f));  // 3/4 of the 80 px handles at the midpoint.
    CHECK(normDeg(d.arena().common(car.node).orientPathDeg) == doctest::Approx(0.0f).epsilon(0.01));
    REQUIRE(d.seekTimeline("", "main", 0.0f) == Status::kOk);
    d.update(0.0f);
    CHECK(normDeg(d.arena().common(car.node).orientPathDeg) == doctest::Approx(90.0f));
}

TEST_CASE("motion-in / motion-out: inline <svg> content and malformed values") {
    Loaded l = load(R"(<xt><head><style>
      @timeline main {
        duration: 1s;
        #Icon/p { 0s { transform: translate(0px, 0px); motion-out: 0px 40px }
                  1s { transform: translate(50px, 0px); motion-in: 0px 40px } }
      }
    </style></head><body>
      <svg id="Icon" viewBox="0 0 100 100" width="100" height="100"><path id="p" d="M0 0 L10 0 L10 10 Z"/></svg>
    </body></xt>)");
    Document& d = *l.doc;
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    bool found = false;
    d.arena().traversePreorder(d.byId("Icon").node, [&](NodeId n) {
        const Vec2 t = d.arena().common(n).translate;
        if (std::fabs(t.x - 25.0f) < 0.01f) {
            CHECK(t.y == doctest::Approx(30.0f));
            found = true;
        }
        return true;
    });
    CHECK(found);

    CompileResult bad = compileMain(R"(<xt><head><style>
      @timeline main { #A { 0s { transform: translate(0px, 0px); motion-out: 1em 2px } } }
    </style></head><body><div id="A"></div></body></xt>)");
    bool error = false;
    for (const Diagnostic& diag : bad.diagnostics) {
        error = error || (diag.level == Diagnostic::Level::Error &&
                          diag.message.find("motion-out") != std::string::npos);
    }
    CHECK(error);
}

TEST_CASE("motion-in / motion-out: .xtanim.json keys carry [inX, inY, outX, outY]") {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;
    std::vector<SourceFile> files{{"main.xt", R"(<xt><head><style>
          #Car { position: absolute; left: 0px; top: 0px; width: 20px; height: 10px; }
          @timeline main from "a.xtanim.json#main";
        </style></head><body><div id="Car"></div></body></xt>)"},
                                  {"a.xtanim.json", R"J({"v": 1, "timelines": [{"name": "main", "tracks": [
          {"layer": "Car", "prop": "transform", "keys": [
            [0, "translate(0px, 0px)", null, [0, 0, 0, 80]],
            [1, "translate(100px, 0px)", "linear", [0, 80, 0, 0]]]}]}]})J"}};
    Loaded l;
    l.result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& diag : l.result.diagnostics) {
        CAPTURE(diag.message);
        CHECK(diag.level != Diagnostic::Level::Error);
    }
    REQUIRE(l.result.ok());
    REQUIRE(Document::load(IrBlob{l.result.ir.data(), l.result.ir.size()}, false, nullptr, nullptr, l.doc) ==
            Status::kOk);
    Document& d = *l.doc;
    d.setViewport(Rect{0, 0, 400, 400});
    d.update(0.0f);
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().element(d.byId("Car").node).cssTranslate.y == doctest::Approx(60.0f));
}

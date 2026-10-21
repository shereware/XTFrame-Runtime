#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "delaunay.h"  // core/src (tests/CMakeLists.txt adds it to the include path).
#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_svg.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/scene_instance.h"
#include "xt/types.h"

/// T-243 (docs/design/32-rigging-and-deformation.md §32.2, §32.4): the rig data model -- Bone /
/// Constraint / Mesh nodes, skins, PropId 246-255, IR minor 5, the `<xt-skeleton|bone|constraint|
/// mesh|pins|tris|weights>` parsing with inverse binds, Delaunay, and `bind:` on scene nodes.

namespace xt {
uint32_t crc32(const uint8_t* data, size_t size);  // core/src/crc32.h (see test_ir_roundtrip.cpp).
}  // namespace xt

using namespace xt;
using namespace xt::compile;

namespace {

struct Parsed {
    IrWriter writer;
    DiagnosticSink sink;
    SceneBuilder scene;
    SvgParseExtras extras;
    std::vector<std::pair<std::string, uint32_t>> ids;
    Status status = Status::kInternal;

    uint32_t node(std::string_view id) const {
        for (const auto& [name, index] : ids) {
            if (name == id) {
                return index;
            }
        }
        FAIL("no node " << id);
        return 0;
    }
    bool warned(std::string_view needle) const {
        for (const Diagnostic& d : sink.all()) {
            if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    bool errored(std::string_view needle) const {
        for (const Diagnostic& d : sink.all()) {
            if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

std::unique_ptr<Parsed> parse(const std::string& body) {
    auto out = std::make_unique<Parsed>();
    const std::string doc = "<svg viewBox=\"0 0 100 100\">" + body + "</svg>";
    out->status = parseSvg(doc, "rig.svg", out->sink, out->writer, out->scene, &out->ids, &out->extras);
    return out;
}

constexpr const char* kSkeleton = R"X(<xt-skeleton id="Rig">
  <xt-bone id="Root" length="50" translate="10 20" rotate="90">
    <xt-bone id="Tip" length="30"/>
  </xt-bone>
</xt-skeleton>)X";

/// Byte `k` of a packed 4 x 8-bit field.
uint32_t byteOf(uint32_t v, int k) { return (v >> (8 * k)) & 0xFFu; }

CompileResult compileXt(const std::string& body, bool strict = false) {
    CompileOptions options;
    options.emitMeta = false;
    options.strict = strict;
    const std::string xt = "<xt><head></head><body id=\"Root\">" + body + "</body></xt>";
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasDiagnostic(const CompileResult& r, Diagnostic::Level level, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

double triArea(const Vec2& a, const Vec2& b, const Vec2& c) {
    return 0.5 * ((double(b.x) - a.x) * (double(c.y) - a.y) - (double(c.x) - a.x) * (double(b.y) - a.y));
}

/// Area of the convex hull of `pts` (monotone chain).
double hullArea(std::vector<Vec2> pts) {
    std::sort(pts.begin(), pts.end(),
              [](const Vec2& a, const Vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<Vec2> h(2 * pts.size());
    size_t k = 0;
    auto cross = [](const Vec2& o, const Vec2& a, const Vec2& b) {
        return (double(a.x) - o.x) * (double(b.y) - o.y) - (double(a.y) - o.y) * (double(b.x) - o.x);
    };
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) {
            --k;
        }
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) {
            --k;
        }
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    double area = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const Vec2& a = h[i];
        const Vec2& b = h[(i + 1) % h.size()];
        area += double(a.x) * b.y - double(b.x) * a.y;
    }
    return 0.5 * area;
}

/// Rewrites a minor-5 `.xtir` as IR minor 4 wrote it: IrPathData records 4 bytes shorter (no
/// skinIndex) and IrBinding records 4 bytes shorter (no sceneNode). Scenes must have no rig tail.
std::vector<uint8_t> downgradeToMinor4(const std::vector<uint8_t>& in) {
    IrHeader header{};
    std::memcpy(&header, in.data(), sizeof(header));
    std::vector<uint8_t> out(in.begin(), in.begin() + sizeof(IrHeader));
    size_t pos = sizeof(IrHeader);
    while (pos + sizeof(IrChunkHeader) <= in.size()) {
        IrChunkHeader ch{};
        std::memcpy(&ch, in.data() + pos, sizeof(ch));
        std::vector<uint8_t> payload(
            in.begin() + static_cast<std::ptrdiff_t>(pos + sizeof(ch)),
            in.begin() + static_cast<std::ptrdiff_t>(pos + sizeof(ch) + ch.payloadLength));
        pos += sizeof(ch) + ch.payloadLength;
        auto cutRecords = [&](size_t countPos, size_t recordSize) {
            uint32_t count = 0;
            std::memcpy(&count, payload.data() + countPos, 4);
            for (uint32_t i = count; i > 0; --i) {
                const size_t end = countPos + 4 + static_cast<size_t>(i) * recordSize;
                payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(end - 4),
                              payload.begin() + static_cast<std::ptrdiff_t>(end));
            }
        };
        if (std::memcmp(ch.type, "SCEN", 4) == 0) {
            uint32_t nodeCount = 0;
            std::memcpy(&nodeCount, payload.data() + 12, 4);
            cutRecords(16 + static_cast<size_t>(nodeCount) * sizeof(IrSceneNode), sizeof(IrPathData));
        } else if (std::memcmp(ch.type, "BIND", 4) == 0) {
            cutRecords(0, sizeof(IrBinding));
        }
        ch.payloadLength = static_cast<uint32_t>(payload.size());
        ch.crc32 = crc32(payload.data(), payload.size());
        const uint8_t* hp = reinterpret_cast<const uint8_t*>(&ch);
        out.insert(out.end(), hp, hp + sizeof(ch));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    header.minor = 4;
    header.totalLength = static_cast<uint32_t>(out.size());
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

}  // namespace

// -- Delaunay (core/src/delaunay.h) -----------------------------------------------------------------

TEST_CASE("rig: Delaunay of a square is 2 CCW triangles, with its center 4") {
    std::vector<Vec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<uint16_t> idx;
    REQUIRE(triangulate(square, idx));
    REQUIRE(idx.size() == 6);
    double area = 0;
    for (size_t t = 0; t < idx.size(); t += 3) {
        const double a = triArea(square[idx[t]], square[idx[t + 1]], square[idx[t + 2]]);
        CHECK(a > 0);
        area += a;
    }
    CHECK(area == doctest::Approx(100.0));

    square.push_back(Vec2{5, 5});
    REQUIRE(triangulate(square, idx));
    REQUIRE(idx.size() == 12);
    for (size_t t = 0; t < idx.size(); t += 3) {
        CHECK(triArea(square[idx[t]], square[idx[t + 1]], square[idx[t + 2]]) == doctest::Approx(25.0));
        CHECK(std::count(idx.begin() + static_cast<std::ptrdiff_t>(t),
                         idx.begin() + static_cast<std::ptrdiff_t>(t) + 3, uint16_t{4}) == 1);
    }
}

TEST_CASE("rig: Delaunay merges duplicates and rejects collinear points") {
    std::vector<Vec2> pts{{0, 0}, {10, 0}, {0, 0}, {10, 10}, {10, 0}};
    std::vector<uint16_t> idx;
    REQUIRE(triangulate(pts, idx));
    REQUIRE(idx.size() == 3);
    for (uint16_t i : idx) {
        CHECK((i == 0 || i == 1 || i == 3));  // Only the first of each coincident set.
    }
    std::vector<Vec2> line{{0, 0}, {1, 1}, {2, 2}, {5, 5}};
    CHECK_FALSE(triangulate(line, idx));
    CHECK(idx.empty());
    std::vector<Vec2> two{{0, 0}, {1, 1}, {0, 0}};
    CHECK_FALSE(triangulate(two, idx));
}

TEST_CASE("rig: Delaunay of 200 random points covers the convex hull without overlap") {
    std::mt19937 rng(243);
    std::uniform_real_distribution<float> dist(0.0f, 500.0f);
    std::vector<Vec2> pts;
    for (int i = 0; i < 200; ++i) {
        pts.push_back(Vec2{dist(rng), dist(rng)});
    }
    std::vector<uint16_t> idx;
    REQUIRE(triangulate(pts, idx));
    REQUIRE(idx.size() % 3 == 0);
    double sum = 0;
    bool allCcw = true;
    for (size_t t = 0; t < idx.size(); t += 3) {
        const double a = triArea(pts[idx[t]], pts[idx[t + 1]], pts[idx[t + 2]]);
        allCcw = allCcw && a > 0;
        sum += a;
    }
    CHECK(allCcw);
    // Positive triangles inside the hull whose areas add up to the hull's cannot overlap.
    const double hull = hullArea(pts);
    CHECK(std::fabs(sum - hull) <= 1e-3 * hull);
    // Euler: a triangulation of n points with h on the hull has 2n - h - 2 triangles; at least
    // every point is used.
    std::vector<bool> used(pts.size(), false);
    for (uint16_t i : idx) {
        used[i] = true;
    }
    CHECK(std::all_of(used.begin(), used.end(), [](bool u) { return u; }));
}

// -- Bones and constraints ----------------------------------------------------------------------

TEST_CASE("rig: <xt-skeleton> / <xt-bone> build a Group and Bone nodes; a child bone sits at the tip") {
    auto p = parse(kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    const uint32_t rig = p->node("Rig");
    const uint32_t root = p->node("Root");
    const uint32_t tip = p->node("Tip");
    const IrSceneNode& rigNode = p->scene.nodes[rig];
    CHECK(rigNode.kind == static_cast<uint8_t>(NodeKind::Group));
    CHECK(rigNode.svgTag == static_cast<uint8_t>(IrSvgTag::XtSkeleton));
    const IrSceneNode& rootNode = p->scene.nodes[root];
    CHECK(rootNode.kind == static_cast<uint8_t>(NodeKind::Bone));
    CHECK(rootNode.svgTag == static_cast<uint8_t>(IrSvgTag::XtBone));
    CHECK(rootNode.parent == rig);
    CHECK(rootNode.translate.x == doctest::Approx(10.0f));
    CHECK(rootNode.translate.y == doctest::Approx(20.0f));
    CHECK(rootNode.rotation == doctest::Approx(90.0f));
    CHECK(p->scene.bones[rootNode.kindIndex].length == doctest::Approx(50.0f));
    const IrSceneNode& tipNode = p->scene.nodes[tip];
    CHECK(tipNode.parent == root);
    CHECK(tipNode.translate.x == doctest::Approx(50.0f));  // The parent's length.
    CHECK(p->scene.bones[tipNode.kindIndex].length == doctest::Approx(30.0f));
    CHECK(irSvgTagCode("xt-bone") == IrSvgTag::XtBone);
    CHECK(irSvgTagCode("xt-mesh") == IrSvgTag::XtMesh);
}

TEST_CASE("rig: a child bone's own translate x warns skin.boneOffset and is replaced") {
    auto p = parse(R"X(<xt-skeleton id="Rig"><xt-bone id="A" length="40">
        <xt-bone id="B" length="10" translate="5 3" scale="2"/></xt-bone></xt-skeleton>)X");
    REQUIRE(p->status == Status::kOk);
    CHECK(p->warned("skin.boneOffset"));
    const IrSceneNode& b = p->scene.nodes[p->node("B")];
    CHECK(b.translate.x == doctest::Approx(40.0f));
    CHECK(b.translate.y == doctest::Approx(0.0f));  // T-243b: y is forced to 0 too.
    CHECK(b.scale.x == doctest::Approx(2.0f));
    CHECK(b.scale.y == doctest::Approx(2.0f));

    auto outside = parse(R"X(<g><xt-bone id="X" length="1"/></g>)X");
    CHECK(outside->warned("<xt-bone> outside <xt-skeleton>"));
    auto child =
        parse(R"X(<xt-skeleton><xt-bone id="A" length="1"><path d="M0 0 L1 1"/></xt-bone></xt-skeleton>)X");
    CHECK(child->warned("inside <xt-bone> is not supported"));
}

TEST_CASE("rig: <xt-constraint> kinds, parameters and targets") {
    auto p = parse(R"X(<xt-skeleton id="Rig"><xt-bone id="A" length="10"><xt-bone id="B" length="10">
          <xt-constraint id="Ik" kind="ik" target="#Hand" strength="0.5" invert="1" mix="0.25" space="world"/>
          <xt-constraint id="Sp" kind="spring" inertia="0.1" strength="0.2" damping="0.3" gravity="40" enabled="0"/>
          <xt-constraint id="Rot" kind="rotation" target="A"/>
        </xt-bone></xt-bone></xt-skeleton><g id="Hand"/>)X");
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    const IrSceneNode& ikNode = p->scene.nodes[p->node("Ik")];
    CHECK(ikNode.kind == static_cast<uint8_t>(NodeKind::Constraint));
    CHECK(ikNode.svgTag == static_cast<uint8_t>(IrSvgTag::XtConstraint));
    CHECK(ikNode.parent == p->node("B"));
    const IrConstraintData& ik = p->scene.constraints[ikNode.kindIndex];
    CHECK(ik.kind == static_cast<uint8_t>(ConstraintKind::Ik));
    CHECK(ik.target == p->node("Hand"));  // Resolved although Hand follows the skeleton.
    CHECK(ik.params[0] == doctest::Approx(0.5f));
    CHECK(ik.params[1] == doctest::Approx(1.0f));
    CHECK(ik.params[2] == doctest::Approx(2.0f));  // chain defaults to 2.
    CHECK(ik.mix == doctest::Approx(0.25f));
    CHECK(ik.space == 1);
    CHECK(ik.enabled == 1);
    const IrConstraintData& sp = p->scene.constraints[p->scene.nodes[p->node("Sp")].kindIndex];
    CHECK(sp.kind == static_cast<uint8_t>(ConstraintKind::Spring));
    CHECK(sp.target == kIrNoTarget);
    CHECK(sp.params[0] == doctest::Approx(0.1f));
    CHECK(sp.params[1] == doctest::Approx(0.2f));
    CHECK(sp.params[2] == doctest::Approx(0.3f));
    CHECK(sp.params[3] == doctest::Approx(40.0f));
    CHECK(sp.enabled == 0);
    const IrConstraintData& rot = p->scene.constraints[p->scene.nodes[p->node("Rot")].kindIndex];
    CHECK(rot.target == p->node("A"));
    CHECK(rot.mix == doctest::Approx(1.0f));

    auto missing =
        parse(R"X(<xt-skeleton><xt-bone length="1"><xt-constraint kind="ik"/></xt-bone></xt-skeleton>)X");
    CHECK(missing->errored("needs a target"));
    auto unknown = parse(
        R"X(<xt-skeleton><xt-bone length="1"><xt-constraint kind="ik" target="#Nope"/></xt-bone></xt-skeleton>)X");
    CHECK(unknown->errored("target \"#Nope\""));
    auto badKind = parse(R"X(<xt-constraint kind="distance" target="#x"/>)X");
    CHECK(badKind->errored("is not ik, transform, rotation or spring"));
}

// -- Skins ------------------------------------------------------------------------------------------

TEST_CASE("rig: a skinned path's weights (=, omitted, unnormalized, unlisted) and inverse binds") {
    // The path comes before the skeleton: skins are resolved once every node exists.
    auto p = parse(
        std::string(
            R"X(<path id="Arm" d="M0 0 L10 0 L10 10" xt-skin="Rig"><xt-weights bones="Root Tip" version="1">
0 0:1
1 0:1 1:3 | = | 1:1
</xt-weights></path>)X") +
        kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    CHECK(p->sink.warningCount() == 0);
    const IrSceneNode& arm = p->scene.nodes[p->node("Arm")];
    const IrPathData& path = p->scene.paths[arm.kindIndex];
    REQUIRE(path.skinIndex == 0);
    REQUIRE(p->scene.skins.size() == 1);
    const IrSkinData& skin = p->scene.skins[0];
    CHECK(skin.skeleton == p->node("Rig"));
    REQUIRE(skin.tendonCount == 2);
    REQUIRE(skin.weightCount == 9);  // 3 vertices x (p, in, out).
    const IrPointWeight* w = &p->scene.weightPool[skin.weightOffset];
    // Vertex 0: p = bone 0 whole; in / out omitted = p.
    for (int c = 0; c < 3; ++c) {
        CHECK(byteOf(w[c].idx4, 0) == 0);
        CHECK(byteOf(w[c].w4, 0) == 255);
        CHECK(byteOf(w[c].w4, 1) == 0);
    }
    // Vertex 1: p = 0:1 1:3 normalized (64 + 191 = 255), in `=` p, out = bone 1 whole.
    for (int c = 0; c < 2; ++c) {
        CHECK(byteOf(w[3 + c].idx4, 0) == 0);
        CHECK(byteOf(w[3 + c].idx4, 1) == 1);
        CHECK(byteOf(w[3 + c].w4, 0) == 64);
        CHECK(byteOf(w[3 + c].w4, 1) == 191);
    }
    CHECK(byteOf(w[5].idx4, 0) == 1);
    CHECK(byteOf(w[5].w4, 0) == 255);
    // Vertex 2: unlisted -> wholly bones[0].
    for (int c = 6; c < 9; ++c) {
        CHECK(byteOf(w[c].idx4, 0) == 0);
        CHECK(byteOf(w[c].w4, 0) == 255);
    }

    // Root rest = T(10, 20) R(90): world (0 1 -1 0 | 10 20); Tip rest = Root x T(50, 0):
    // (0 1 -1 0 | 10 70). Inverses: Root (0 -1 1 0 | -20 10), Tip (0 -1 1 0 | -70 10).
    const IrTendon& root = p->scene.tendonPool[skin.tendonOffset];
    const IrTendon& tip = p->scene.tendonPool[skin.tendonOffset + 1];
    CHECK(root.bone == p->node("Root"));
    CHECK(tip.bone == p->node("Tip"));
    const float rootInv[6] = {0, -1, 1, 0, -20, 10};
    const float tipInv[6] = {0, -1, 1, 0, -70, 10};
    for (int k = 0; k < 6; ++k) {
        CAPTURE(k);
        CHECK(root.inverseBind[k] == doctest::Approx(rootInv[k]).epsilon(1e-4));
        CHECK(tip.inverseBind[k] == doctest::Approx(tipInv[k]).epsilon(1e-4));
    }
    // Inverse bind x rest world = identity.
    const Mat2x3 tipWorld{0, 1, -1, 0, 10, 70};
    const Mat2x3 inv{tip.inverseBind[0], tip.inverseBind[1], tip.inverseBind[2],
                     tip.inverseBind[3], tip.inverseBind[4], tip.inverseBind[5]};
    CHECK(approxEqual(mul(inv, tipWorld), Mat2x3{}, 1e-4f));
}

TEST_CASE("rig: the shape's own transform is folded into the inverse bind") {
    auto p = parse(std::string(R"X(<path id="P" d="M0 0 L1 0" transform="translate(5 0)" xt-skin="#Rig">
        <xt-weights bones="Root">0 0:1</xt-weights></path>)X") +
                   kSkeleton);
    REQUIRE(p->status == Status::kOk);
    const IrTendon& t = p->scene.tendonPool[0];
    // T-243b: inverse(Root rest) x T(5, 0) = (0 -1 1 0 | -20 10) x T(5, 0) -> e = -20, f = 5.
    CHECK(t.inverseBind[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(t.inverseBind[1] == doctest::Approx(-1.0f).epsilon(1e-4));
    CHECK(t.inverseBind[2] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(t.inverseBind[3] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(t.inverseBind[4] == doctest::Approx(-20.0f).epsilon(1e-4));
    CHECK(t.inverseBind[5] == doctest::Approx(5.0f).epsilon(1e-4));
    // At rest: world(Root) x inverseBind = the own transform.
    const Mat2x3 rest{0, 1, -1, 0, 10, 20};
    const Mat2x3 inv{t.inverseBind[0], t.inverseBind[1], t.inverseBind[2],
                     t.inverseBind[3], t.inverseBind[4], t.inverseBind[5]};
    CHECK(approxEqual(mul(rest, inv), translation(Vec2{5, 0}), 1e-4f));
}

TEST_CASE("rig: <xt-skeleton> takes no transform (skin.skeletonTransform); rest stops at the group") {
    for (const char* attr :
         {"transform=\"translate(30 40)\"", "translate=\"30 40\"", "rotate=\"45\"", "scale=\"2\""}) {
        CAPTURE(attr);
        auto p = parse(
            std::string(
                R"X(<path id="P" d="M0 0 L1 0" xt-skin="Rig"><xt-weights bones="Root">0 0:1</xt-weights></path>
            <xt-skeleton id="Rig" )X") +
            attr + R"X(><xt-bone id="Root" length="50" translate="10 20" rotate="90"/></xt-skeleton>)X");
        REQUIRE(p->status == Status::kOk);
        CHECK(p->warned("skin.skeletonTransform"));
        const IrSceneNode& rig = p->scene.nodes[p->node("Rig")];
        CHECK(rig.translate.x == doctest::Approx(0.0f));
        CHECK(rig.translate.y == doctest::Approx(0.0f));
        CHECK(rig.rotation == doctest::Approx(0.0f));
        CHECK(rig.scale.x == doctest::Approx(1.0f));
        // The rest world is the bone's own (0 1 -1 0 | 10 20): inverse (0 -1 1 0 | -20 10).
        const IrTendon& t = p->scene.tendonPool[0];
        CHECK(t.inverseBind[4] == doctest::Approx(-20.0f).epsilon(1e-4));
        CHECK(t.inverseBind[5] == doctest::Approx(10.0f).epsilon(1e-4));
    }
    // A skeleton inside a transformed <g>: the group is outside the skeleton, so not in the rest.
    auto nested =
        parse(R"X(<path id="P" d="M0 0 L1 0" xt-skin="Rig"><xt-weights bones="Root">0 0:1</xt-weights></path>
        <g transform="translate(100 0)"><xt-skeleton id="Rig"><xt-bone id="Root" length="5" translate="10 20"/></xt-skeleton></g>)X");
    REQUIRE(nested->status == Status::kOk);
    CHECK_FALSE(nested->warned("skin.skeletonTransform"));
    CHECK(nested->scene.tendonPool[0].inverseBind[4] == doctest::Approx(-10.0f).epsilon(1e-4));
    CHECK(nested->scene.tendonPool[0].inverseBind[5] == doctest::Approx(-20.0f).epsilon(1e-4));
}

// -- T-243c: weight rows by on-curve vertex ---------------------------------------------------------

namespace {

/// The bone index of the heaviest (first) item of weight slot `slot`, column `c` (0 p, 1 in, 2 out).
uint32_t slotBone(const Parsed& p, uint32_t slot, uint32_t c) {
    const IrSkinData& skin = p.scene.skins[0];
    return byteOf(p.scene.weightPool[skin.weightOffset + slot * 3 + c].idx4, 0);
}

}  // namespace

TEST_CASE("rig: a closed rectangle ending in a curve back to the start has 4 weight rows") {
    auto p = parse(std::string(R"X(<path id="P" d="M0 0 L10 0 L10 10 L0 10 C0 7 0 3 0 0 Z" xt-skin="Rig">
<xt-weights bones="Root Tip">
0 0:1 | 1:1 | 0:1
1 1:1
2 0:1
3 1:1
4 1:1
</xt-weights></path>)X") +
                   kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK(p->warned("vertex 4 is out of range (4 vertices)"));
    CHECK(p->sink.warningCount() == 1);
    const IrSkinData& skin = p->scene.skins[0];
    REQUIRE(skin.weightCount == 15);  // 5 non-Close verbs x (p, in, out): the IR layout is unchanged.
    CHECK(slotBone(*p, 0, 0) == 0);
    CHECK(slotBone(*p, 0, 1) == 1);
    CHECK(slotBone(*p, 1, 0) == 1);
    CHECK(slotBone(*p, 2, 0) == 0);
    CHECK(slotBone(*p, 3, 0) == 1);
    // The closing curve's end is vertex 0: p and in (and out) from vertex 0's row.
    CHECK(slotBone(*p, 4, 0) == 0);
    CHECK(slotBone(*p, 4, 1) == 1);
    CHECK(slotBone(*p, 4, 2) == 0);
}

TEST_CASE("rig: switching a closed contour's last segment between Z, a line and a curve keeps the rows") {
    for (const char* d :
         {"M0 0 L10 0 L10 10 L0 10 Z", "M0 0 L10 0 L10 10 L0 10 L0 0 Z",
          "M0 0 L10 0 L10 10 L0 10 C0 7 0 3 0 0 Z", "M0 0 C3 0 7 0 10 0 L10 10 L0 10 L0 0Z"}) {
        CAPTURE(d);
        auto p =
            parse(std::string("<path id=\"P\" d=\"") + d + R"X(" xt-skin="Rig"><xt-weights bones="Root Tip">
0 0:1
1 1:1
2 0:1
3 1:1
</xt-weights></path>)X" +
                  kSkeleton);
        REQUIRE(p->status == Status::kOk);
        CHECK(p->sink.warningCount() == 0);
        const IrSkinData& skin = p->scene.skins[0];
        const uint32_t slots = skin.weightCount / 3;
        REQUIRE(slots >= 4);
        for (uint32_t v = 0; v < 4; ++v) {
            CHECK(slotBone(*p, v, 0) == v % 2);
        }
        if (slots == 5) {
            CHECK(slotBone(*p, 4, 0) == 0);  // The closing end = vertex 0.
        }
    }
}

TEST_CASE("rig: contours continue the vertex count; an end off the start point is its own vertex") {
    // Contour 1: 3 vertices (its closing L0 0 is vertex 0); contour 2: 3 more; contour 3 (open,
    // ending on its start point) keeps that end as a vertex: 3 + 3 + 3 = 9 vertices, 10 slots.
    auto p = parse(
        std::string(
            R"X(<path id="P" d="M0 0 L10 0 L10 10 L0 0 Z M20 0 L30 0 L30 10 Z M40 0 L50 0 L40 0" xt-skin="Rig">
<xt-weights bones="Root Tip">
2 1:1
3 1:1
5 1:1
8 1:1
9 1:1
</xt-weights></path>)X") +
        kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK(p->warned("vertex 9 is out of range (9 vertices)"));
    const IrSkinData& skin = p->scene.skins[0];
    REQUIRE(skin.weightCount == 30);
    const uint32_t want[10] = {0, 0, 1, 0, 1, 0, 1, 0, 0, 1};  // Bone per slot.
    for (uint32_t s = 0; s < 10; ++s) {
        CAPTURE(s);
        CHECK(slotBone(*p, s, 0) == want[s]);
    }
}

TEST_CASE("rig: malformed weight lines warn and are dropped") {
    auto p = parse(std::string(R"X(<path id="P" d="M0 0 L1 0 L1 1" xt-skin="Rig"><xt-weights bones="Root Tip">
5 0:1
1 2:1
2 0:1 1:1 0:1 1:1 0:1
0 1:0
1 1:2
</xt-weights></path>)X") +
                   kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    CHECK(p->warned("vertex 5 is out of range"));
    CHECK(p->warned("a bone index is out of range"));
    CHECK(p->warned("malformed weights"));
    CHECK(p->sink.warningCount() == 4);  // + the zero-sum line.
    const IrSkinData& skin = p->scene.skins[0];
    const IrPointWeight* w = &p->scene.weightPool[skin.weightOffset];
    CHECK(byteOf(w[0].idx4, 0) == 0);  // Vertex 0's zero-sum line was dropped: bones[0].
    CHECK(byteOf(w[3].idx4, 0) == 1);  // Vertex 1 = 1:2 -> bone 1 whole.
    CHECK(byteOf(w[3].w4, 0) == 255);
}

TEST_CASE("rig: skin references are checked") {
    auto noSkeleton = parse(
        R"X(<g id="G"/><path d="M0 0 L1 0" xt-skin="G"><xt-weights bones="A">0 0:1</xt-weights></path>)X");
    CHECK(noSkeleton->errored("is not an <xt-skeleton>"));
    auto badBone = parse(
        std::string(
            R"X(<path d="M0 0 L1 0" xt-skin="Rig"><xt-weights bones="Root Nope">0 0:1</xt-weights></path>)X") +
        kSkeleton);
    CHECK(badBone->errored("bone \"Nope\""));
    auto noWeights = parse(std::string(R"X(<path d="M0 0 L1 0" xt-skin="Rig"/>)X") + kSkeleton);
    CHECK(noWeights->warned("xt-skin without <xt-weights>"));
    auto shape = parse(std::string(R"X(<rect width="4" height="4" xt-skin="Rig"/>)X") + kSkeleton);
    CHECK(shape->warned("skin.shape"));
}

// -- Meshes -----------------------------------------------------------------------------------------

TEST_CASE("rig: <xt-mesh> pins, uvs, Delaunay triangles and an asset entry") {
    auto p =
        parse(R"X(<xt-mesh id="Face" href="img/face.png" width="200" height="100" opacity="0.5" smooth="0">
        <xt-pins>0,0 200,0 200,100 0,100 250,50</xt-pins></xt-mesh>)X");
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    const IrSceneNode& n = p->scene.nodes[p->node("Face")];
    CHECK(n.kind == static_cast<uint8_t>(NodeKind::Mesh));
    CHECK(n.svgTag == static_cast<uint8_t>(IrSvgTag::XtMesh));
    const IrMeshData& m = p->scene.meshes[n.kindIndex];
    REQUIRE(m.vertexCount == 5);
    CHECK(m.opacity == doctest::Approx(0.5f));
    CHECK(m.smooth == 0);
    CHECK(m.skinIndex == kIrNoSkin);
    REQUIRE(m.asset < p->writer.assets().size());
    CHECK(p->writer.stringAt(p->writer.assets()[m.asset].name) == "img/face.png");
    const Vec2* pos = &p->scene.meshVec2Pool[m.posOffset];
    const Vec2* uv = pos + m.vertexCount;
    CHECK(pos[4].x == doctest::Approx(250.0f));
    CHECK(uv[1].x == doctest::Approx(1.0f));
    CHECK(uv[1].y == doctest::Approx(0.0f));
    CHECK(uv[2].y == doctest::Approx(1.0f));
    CHECK(uv[4].x == doctest::Approx(1.0f));  // Clamped: the pin is outside the image.
    CHECK(uv[4].y == doctest::Approx(0.5f));
    REQUIRE(m.indexCount == 9);
    double area = 0;
    for (uint32_t t = 0; t < m.indexCount; t += 3) {
        const uint16_t* i = &p->scene.meshIndexPool[m.indexOffset + t];
        area += triArea(pos[i[0]], pos[i[1]], pos[i[2]]);
    }
    CHECK(area == doctest::Approx(22500.0));
}

TEST_CASE("rig: <xt-mesh> defaults to the four corners; explicit and bad <xt-tris>; too few pins") {
    auto corners = parse(R"X(<xt-mesh id="M" href="a.png" width="10" height="20"/>)X");
    REQUIRE(corners->status == Status::kOk);
    const IrMeshData& m = corners->scene.meshes[0];
    CHECK(m.vertexCount == 4);
    CHECK(m.indexCount == 6);
    CHECK(corners->scene.meshVec2Pool[2].x == doctest::Approx(10.0f));
    CHECK(corners->scene.meshVec2Pool[2].y == doctest::Approx(20.0f));

    auto tris =
        parse(R"X(<xt-mesh href="a.png" width="10" height="10"><xt-tris>0 1 2 0 2 3</xt-tris></xt-mesh>)X");
    REQUIRE(tris->scene.meshes.size() == 1);
    CHECK(std::vector<uint16_t>(tris->scene.meshIndexPool.begin(), tris->scene.meshIndexPool.end()) ==
          std::vector<uint16_t>{0, 1, 2, 0, 2, 3});

    auto badIndex =
        parse(R"X(<xt-mesh href="a.png" width="10" height="10"><xt-tris>0 1 9</xt-tris></xt-mesh>)X");
    CHECK(badIndex->errored("<xt-tris> index 9"));
    auto badCount =
        parse(R"X(<xt-mesh href="a.png" width="10" height="10"><xt-tris>0 1</xt-tris></xt-mesh>)X");
    CHECK(badCount->errored("<xt-tris> is not a list of index triples"));
    auto fewPins =
        parse(R"X(<xt-mesh href="a.png" width="10" height="10"><xt-pins>0,0 1,1</xt-pins></xt-mesh>)X");
    CHECK(fewPins->errored("at least 3 pins"));
    auto collinear =
        parse(R"X(<xt-mesh href="a.png" width="10" height="10"><xt-pins>0,0 1,1 2,2</xt-pins></xt-mesh>)X");
    CHECK(collinear->errored("mesh.degenerate"));
    auto noSize = parse(R"X(<xt-mesh href="a.png"/>)X");
    CHECK(noSize->errored("positive width and height"));
}

TEST_CASE("rig: a skinned mesh has one weight per pin") {
    auto p = parse(std::string(R"X(<xt-mesh id="M" href="a.png" width="10" height="10" xt-skin="#Rig">
        <xt-weights bones="Root Tip">2 1:1</xt-weights></xt-mesh>)X") +
                   kSkeleton);
    REQUIRE(p->status == Status::kOk);
    CHECK_FALSE(p->sink.hasErrors());
    const IrMeshData& m = p->scene.meshes[0];
    REQUIRE(m.skinIndex == 0);
    const IrSkinData& skin = p->scene.skins[0];
    CHECK(skin.weightCount == 4);
    CHECK(byteOf(p->scene.weightPool[skin.weightOffset + 2].idx4, 0) == 1);
    auto extra = parse(std::string(R"X(<xt-mesh href="a.png" width="10" height="10" xt-skin="Rig">
        <xt-weights bones="Root">0 0:1 | 0:1</xt-weights></xt-mesh>)X") +
                       kSkeleton);
    CHECK(extra->warned("a mesh vertex has only the p column"));
}

// -- Properties -------------------------------------------------------------------------------------

TEST_CASE("rig: PropId 246-255 apply to Bone / Constraint; get/setProperty") {
    CHECK(propertyApplies(NodeKind::Bone, prop::BoneLength));
    CHECK(propertyApplies(NodeKind::Bone, prop::Rotation));
    CHECK(propertyApplies(NodeKind::Bone, prop::Translate));
    CHECK(propertyApplies(NodeKind::Bone, prop::Visible));
    CHECK_FALSE(propertyApplies(NodeKind::Bone, prop::Opacity));
    CHECK_FALSE(propertyApplies(NodeKind::Bone, prop::IkStrength));
    CHECK_FALSE(propertyApplies(NodeKind::Path, prop::BoneLength));
    CHECK(propertyApplies(NodeKind::Constraint, prop::IkStrength));
    CHECK(propertyApplies(NodeKind::Constraint, prop::ConstraintEnabled));
    CHECK_FALSE(propertyApplies(NodeKind::Constraint, prop::Rotation));
    CHECK(propertyApplies(NodeKind::Mesh, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Mesh, prop::ScaleX));
    CHECK_FALSE(propertyApplies(NodeKind::Mesh, prop::FillColor));
    CHECK(std::string(propertyName(prop::ConstraintParam3)) == "ConstraintParam3");
    CHECK(propertyByName("BoneLength") == prop::BoneLength);
    CHECK(propertyType(prop::ConstraintMix) == ValueType::Float);
    CHECK(propertyDirtyBits(prop::BoneLength) == static_cast<uint8_t>(kDirtyTransform));

    NodeArena arena;
    const NodeId bone = arena.create(NodeKind::Bone);
    setProperty(arena, bone, prop::BoneLength, Value::ofFloat(12.0f));
    CHECK(arena.bone(bone).length == doctest::Approx(12.0f));
    CHECK(getProperty(arena, bone, prop::BoneLength).f == doctest::Approx(12.0f));

    const NodeId ik = arena.create(NodeKind::Constraint);
    setProperty(arena, ik, prop::IkStrength, Value::ofFloat(0.75f));
    CHECK(arena.constraint(ik).params[0] == doctest::Approx(0.75f));
    CHECK(getProperty(arena, ik, prop::ConstraintParam0).f == doctest::Approx(0.75f));
    setProperty(arena, ik, prop::ConstraintMix, Value::ofFloat(0.5f));
    setProperty(arena, ik, prop::ConstraintEnabled, Value::ofFloat(0.0f));
    CHECK(arena.constraint(ik).mix == doctest::Approx(0.5f));
    CHECK(arena.constraint(ik).enabled == 0);

    const NodeId spring = arena.create(NodeKind::Constraint);
    arena.constraint(spring).kind = ConstraintKind::Spring;
    setProperty(arena, spring, prop::IkStrength, Value::ofFloat(9.0f));  // Ik only: ignored.
    CHECK(arena.constraint(spring).params[0] == 0.0f);
    CHECK(getProperty(arena, spring, prop::IkStrength).f == 0.0f);
    setProperty(arena, spring, prop::ConstraintParam3, Value::ofFloat(98.0f));
    CHECK(arena.constraint(spring).params[3] == doctest::Approx(98.0f));

    const NodeId mesh = arena.create(NodeKind::Mesh);
    CHECK(arena.mesh(mesh).skin.skeleton == kInvalidNode);
}

// -- IR -----------------------------------------------------------------------------------------------

namespace {

const char* kRigDoc = R"X(<div port:ang="number" port:len="number" port:mix="number"></div>
<svg id="S" viewBox="0 0 100 100">
  <path id="Arm" d="M0 0 L10 0 L10 10" xt-skin="Rig"><xt-weights bones="Root Tip">1 0:1 1:1</xt-weights></path>
  <xt-skeleton id="Rig">
    <xt-bone id="Root" length="50" translate="10 20" rotate="90" bind:rotate="ang">
      <xt-bone id="Tip" length="30" bind:length="len">
        <xt-constraint id="Ik" kind="ik" target="#Hand" bind:mix="mix"/>
      </xt-bone>
    </xt-bone>
  </xt-skeleton>
  <g id="Hand"/>
  <xt-mesh id="Face" href="face.png" width="20" height="20" xt-skin="Rig"><xt-weights bones="Tip">0 0:1</xt-weights></xt-mesh>
</svg>)X";

}  // namespace

TEST_CASE("rig: IR 2.5 round-trips byte for byte and instantiates the rig") {
    CompileResult r = compileXt(kRigDoc);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    IrHeader header{};
    std::memcpy(&header, r.ir.data(), sizeof(header));
    CHECK(header.minor == kIrFormatMinor);
    CHECK(kIrFormatMinor >= 5);  // T-335: 6.
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    CHECK(IrWriter::fromView(view.value()).finish() == r.ir);

    // The scene: bones, a constraint, a mesh, two skins.
    const SceneChunk* svg = nullptr;
    for (uint32_t i = 0; i < view.value().sceneCount(); ++i) {
        if (!view.value().scene(i).bones().empty()) {
            svg = &view.value().scene(i);
        }
    }
    REQUIRE(svg != nullptr);
    CHECK(svg->bones().size() == 2);
    CHECK(svg->constraints().size() == 1);
    CHECK(svg->meshes().size() == 1);
    CHECK(svg->skins().size() == 2);
    CHECK(svg->tendonPool().size() == 3);
    CHECK(svg->weightPool().size() == 9 + 4);
    CHECK(svg->meshVec2Pool().size() == 8);
    CHECK(svg->meshIndexPool().size() == 6);

    // Three scene-node bindings on the <svg> element.
    uint32_t sceneBindings = 0;
    for (const IrBinding& b : view.value().bindings().bindings()) {
        if (b.sceneNode != kBindElementSelf) {
            ++sceneBindings;
            CHECK(static_cast<BindTarget>(b.target) == BindTarget::Prop);
        }
    }
    CHECK(sceneBindings == 3);

    // instantiateScene: kinds, data, skin and constraint target in arena NodeIds.
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 200});
    doc->update(0.0f);
    const SceneInstance* inst = doc->sceneInstance(doc->byId("S"));
    REQUIRE(inst != nullptr);
    const NodeArena& arena = doc->arena();
    NodeId arm = kInvalidNode, root = kInvalidNode, tip = kInvalidNode, ik = kInvalidNode,
           mesh = kInvalidNode, hand = kInvalidNode, rig = kInvalidNode;
    for (NodeId id : inst->localToArena) {
        const std::string_view name = arena.name(id) != kInvalidString ? doc->string(arena.name(id)) : "";
        if (name == "Arm") arm = id;
        if (name == "Root") root = id;
        if (name == "Tip") tip = id;
        if (name == "Ik") ik = id;
        if (name == "Face") mesh = id;
        if (name == "Hand") hand = id;
        if (name == "Rig") rig = id;
    }
    REQUIRE(arm != kInvalidNode);
    REQUIRE(mesh != kInvalidNode);
    CHECK(arena.kind(root) == NodeKind::Bone);
    CHECK(arena.kind(ik) == NodeKind::Constraint);
    CHECK(arena.kind(mesh) == NodeKind::Mesh);
    CHECK(svg->bones()[1].length == doctest::Approx(30.0f));
    CHECK(arena.bone(tip).length == 0.0f);  // bind:length already wrote port `len`'s initial 0.
    CHECK(arena.constraint(ik).target == hand);
    const SkinData& armSkin = arena.path(arm).skin;
    CHECK(armSkin.skeleton == rig);
    REQUIRE(armSkin.tendons.size() == 2);
    CHECK(armSkin.tendons[0].bone == root);
    CHECK(armSkin.tendons[1].bone == tip);
    CHECK(armSkin.tendons[1].inverseBind.e == doctest::Approx(-70.0f).epsilon(1e-4));
    CHECK(armSkin.weights.size() == 9);
    const MeshNodeData& face = arena.mesh(mesh);
    CHECK(face.positions.size() == 4);
    CHECK(face.uvs[2].x == doctest::Approx(1.0f));
    CHECK(face.indices.size() == 6);
    CHECK(face.skin.skeleton == rig);
    REQUIRE(face.skin.tendons.size() == 1);
    CHECK(face.skin.tendons[0].bone == tip);
}

TEST_CASE("rig: bind:rotate / bind:length / bind:mix write scene nodes through ports") {
    CompileResult r = compileXt(kRigDoc);
    REQUIRE(r.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 200});
    doc->update(0.0f);
    CHECK(doc->warnings().empty());
    const SceneInstance* inst = doc->sceneInstance(doc->byId("S"));
    REQUIRE(inst != nullptr);
    NodeArena& arena = doc->arena();
    NodeId root = kInvalidNode, tip = kInvalidNode, ik = kInvalidNode;
    for (NodeId id : inst->localToArena) {
        const std::string_view name = arena.name(id) != kInvalidString ? doc->string(arena.name(id)) : "";
        if (name == "Root") root = id;
        if (name == "Tip") tip = id;
        if (name == "Ik") ik = id;
    }
    REQUIRE(root != kInvalidNode);
    REQUIRE(doc->setPort(doc->lookupPort("ang"), Value::ofFloat(45.0f)) == Status::kOk);
    REQUIRE(doc->setPort(doc->lookupPort("len"), Value::ofFloat(12.0f)) == Status::kOk);
    REQUIRE(doc->setPort(doc->lookupPort("mix"), Value::ofFloat(0.25f)) == Status::kOk);
    doc->update(0.016f);
    CHECK(getProperty(arena, root, prop::Rotation).f == doctest::Approx(45.0f));
    CHECK(getProperty(arena, tip, prop::BoneLength).f == doctest::Approx(12.0f));
    CHECK(getProperty(arena, ik, prop::ConstraintMix).f == doctest::Approx(0.25f));
    REQUIRE(doc->setPort(doc->lookupPort("ang"), Value::ofFloat(-30.0f)) == Status::kOk);
    doc->update(0.016f);  // T-389: evaluated by the next update().
    CHECK(getProperty(arena, root, prop::Rotation).f == doctest::Approx(-30.0f));
}

TEST_CASE("rig: bind: on <svg> content binds only properties the node takes; on: warns") {
    CompileResult text = compileXt(R"X(<div port:t="string"></div><svg viewBox="0 0 10 10">
        <xt-skeleton><xt-bone length="1" bind:text="t"/></xt-skeleton></svg>)X");
    CHECK_FALSE(text.ok());
    CHECK(hasDiagnostic(text, Diagnostic::Level::Error, "only a property binds on <svg> content"));
    CompileResult opacity = compileXt(R"X(<div port:o="number"></div><svg viewBox="0 0 10 10">
        <xt-skeleton><xt-bone length="1" bind:opacity="o"/></xt-skeleton></svg>)X");
    CHECK_FALSE(opacity.ok());
    CHECK(hasDiagnostic(opacity, Diagnostic::Level::Error, "does not apply to this <svg> node"));
    CompileResult spring = compileXt(R"X(<div port:s="number"></div><svg viewBox="0 0 10 10">
        <xt-skeleton><xt-bone length="1"><xt-constraint kind="spring" bind:strength="s"/></xt-bone></xt-skeleton></svg>)X");
    REQUIRE(spring.ok());
    Result<IrView> view = IrView::open(IrBlob{spring.ir.data(), spring.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    REQUIRE(view.value().bindings().bindings().size() == 1);
    CHECK(view.value().bindings().bindings()[0].targetId == prop::ConstraintParam1);  // A spring's strength.
    CompileResult on =
        compileXt(R"X(<svg viewBox="0 0 10 10"><path d="M0 0 L1 1" on:click="emit(x)"/></svg>)X");
    CHECK(hasDiagnostic(on, Diagnostic::Level::Warning, "on:click on <svg> content is not supported"));
}

TEST_CASE("rig: an IR minor 4 file (24-byte bindings, 152-byte paths) opens with no skin / scene node") {
    CompileResult r = compileXt(R"X(<div port:w="number"></div><span id="T" bind:opacity="w">x</span>
        <svg id="S" viewBox="0 0 10 10"><path id="P" d="M0 0 L5 0 L5 5Z" fill="red"/></svg>)X");
    REQUIRE(r.ok());
    std::vector<uint8_t> old = downgradeToMinor4(r.ir);
    REQUIRE(old.size() < r.ir.size());
    Result<IrView> view = IrView::open(IrBlob{old.data(), old.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    bool sawPath = false;
    for (uint32_t i = 0; i < view.value().sceneCount(); ++i) {
        for (const IrPathData& p : view.value().scene(i).paths()) {
            CHECK(p.skinIndex == kIrNoSkin);
            CHECK(p.strokeWidth == doctest::Approx(1.0f));
            sawPath = true;
        }
    }
    CHECK(sawPath);
    REQUIRE(view.value().bindings().bindings().size() == 1);
    CHECK(view.value().bindings().bindings()[0].sceneNode == kBindElementSelf);
    CHECK(static_cast<BindTarget>(view.value().bindings().bindings()[0].target) == BindTarget::Prop);

    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{old.data(), old.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 100, 100});
    doc->update(0.0f);
    REQUIRE(doc->setPort(doc->lookupPort("w"), Value::ofFloat(0.5f)) == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->byId("T").property(prop::Opacity).f == doctest::Approx(0.5f));
}

TEST_CASE("rig: xtframe-compile --strict fails on a missing target and on out-of-range weights") {
    const std::string missing = R"X(<svg viewBox="0 0 10 10"><xt-skeleton><xt-bone length="1">
        <xt-constraint kind="ik"/></xt-bone></xt-skeleton></svg>)X";
    CHECK_FALSE(compileXt(missing).ok());  // An error even without --strict.
    CHECK_FALSE(compileXt(missing, true).ok());

    const std::string weights = R"X(<svg viewBox="0 0 10 10"><path d="M0 0 L1 0" xt-skin="Rig">
        <xt-weights bones="B">0 0:1
        7 0:1</xt-weights></path><xt-skeleton id="Rig"><xt-bone id="B" length="1"/></xt-skeleton></svg>)X";
    CompileResult lax = compileXt(weights);
    CHECK(lax.ok());
    CHECK(hasDiagnostic(lax, Diagnostic::Level::Warning, "vertex 7 is out of range"));
    CHECK_FALSE(compileXt(weights, true).ok());
}

TEST_CASE("rig: named bones declare <id>.world / <id>.angle out ports after the system ports (T-243b)") {
    CompileResult r = compileXt(R"X(<div bind:text="system.language"></div>
<svg id="S1" viewBox="0 0 10 10"><xt-skeleton id="Rig"><xt-bone id="A" length="4"><xt-bone length="2"/>
  <xt-bone id="B" length="2"/></xt-bone></xt-skeleton></svg>
<svg id="S2" viewBox="0 0 10 10"><xt-skeleton id="Rig2"><xt-bone id="A" length="1"/></xt-skeleton></svg>)X");
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    // The second "A" warns once per port and declares nothing.
    CHECK(hasDiagnostic(r, Diagnostic::Level::Warning, "bone port \"A.world\" is already declared"));
    CHECK(hasDiagnostic(r, Diagnostic::Level::Warning, "bone port \"A.angle\" is already declared"));
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    CHECK(IrWriter::fromView(view.value()).finish() == r.ir);  // The IR round-trips byte for byte.
    const auto ports = view.value().ports().ports();
    REQUIRE(ports.size() == 5);
    CHECK(view.value().string(ports[0].name) == "system.language");
    const char* names[4] = {"A.world", "A.angle", "B.world", "B.angle"};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const IrPort& p = ports[static_cast<size_t>(i) + 1];
        CHECK(view.value().string(p.name) == names[i]);
        CHECK(static_cast<PortValueType>(p.type) ==
              (i % 2 == 0 ? PortValueType::Vec2 : PortValueType::Number));
        CHECK(static_cast<PortDir>(p.dir) == PortDir::Out);
        CHECK(p.element == 0u);
        CHECK(p.scope == kNoPortScope);
        CHECK(p.fieldCount == 0u);
    }
}

namespace {

/// `in` with the last `drop` ports of its PORT chunk cut (a file compiled before T-243b, whose
/// bone ports were not declared).
std::vector<uint8_t> dropTrailingPorts(const std::vector<uint8_t>& in, uint32_t drop) {
    IrHeader header{};
    std::memcpy(&header, in.data(), sizeof(header));
    std::vector<uint8_t> out(in.begin(), in.begin() + sizeof(IrHeader));
    size_t pos = sizeof(IrHeader);
    while (pos + sizeof(IrChunkHeader) <= in.size()) {
        IrChunkHeader ch{};
        std::memcpy(&ch, in.data() + pos, sizeof(ch));
        std::vector<uint8_t> payload(
            in.begin() + static_cast<std::ptrdiff_t>(pos + sizeof(ch)),
            in.begin() + static_cast<std::ptrdiff_t>(pos + sizeof(ch) + ch.payloadLength));
        pos += sizeof(ch) + ch.payloadLength;
        if (std::memcmp(ch.type, "PORT", 4) == 0) {
            uint32_t count = 0;
            std::memcpy(&count, payload.data(), 4);
            REQUIRE(count >= drop);
            const size_t end = 4 + static_cast<size_t>(count) * sizeof(IrPort);
            payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(end - drop * sizeof(IrPort)),
                          payload.begin() + static_cast<std::ptrdiff_t>(end));
            count -= drop;
            std::memcpy(payload.data(), &count, 4);
        }
        ch.payloadLength = static_cast<uint32_t>(payload.size());
        ch.crc32 = crc32(payload.data(), payload.size());
        const uint8_t* hp = reinterpret_cast<const uint8_t*>(&ch);
        out.insert(out.end(), hp, hp + sizeof(ch));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    header.totalLength = static_cast<uint32_t>(out.size());
    std::memcpy(out.data(), &header, sizeof(header));
    return out;
}

}  // namespace

TEST_CASE("rig: the run time takes the declared bone ports; a file without them still gets them") {
    const char* body =
        R"X(<svg id="S" viewBox="0 0 100 100" style="position: absolute; left: 0; top: 0; width: 100px; height: 100px">
  <xt-skeleton id="Rig"><xt-bone id="A" length="10" translate="20 30"/></xt-skeleton></svg>)X";
    CompileResult r = compileXt(body);
    REQUIRE(r.ok());
    for (int variant = 0; variant < 2; ++variant) {
        CAPTURE(variant);
        const std::vector<uint8_t> ir = variant == 0 ? r.ir : dropTrailingPorts(r.ir, 2);
        Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{true});
        REQUIRE(view.ok());
        CHECK(view.value().ports().ports().size() == (variant == 0 ? 2u : 0u));
        std::unique_ptr<Document> doc;
        REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
        doc->setViewport(Rect{0, 0, 100, 100});
        const PortId world = doc->lookupPort("A.world");
        const PortId angle = doc->lookupPort("A.angle");
        REQUIRE(world != kInvalidPort);
        REQUIRE(angle != kInvalidPort);
        CHECK(world == 1u);  // Declared: the PORT index; added at load: after the (no) IR ports.
        CHECK(doc->portInfo(world).dir == PortDir::Out);
        CHECK(doc->portInfo(world).type == PortValueType::Vec2);
        CHECK(doc->portInfo(angle).type == PortValueType::Number);
        doc->update(0.0f);
        CHECK(doc->getPort(world).v2.x == doctest::Approx(20.0f));
        CHECK(doc->getPort(world).v2.y == doctest::Approx(30.0f));
    }
}

TEST_CASE("rig: rest-* is the bind pose, the plain attributes the pose (T-249)") {
    // Root rests at T(10, 20) (rest-rotate 0) and is posed at rotate 90: the shape loads posed.
    const std::string body = R"X(<svg id="S" viewBox="0 0 200 200">
      <xt-skeleton id="Rig"><xt-bone id="Root" length="50" translate="10 20" rotate="90" rest-rotate="0"/></xt-skeleton>
      <path id="P" d="M20 20 L60 20 L60 40" xt-skin="Rig"><xt-weights bones="Root">0 0:1
1 0:1
2 0:1</xt-weights></path></svg>)X";
    CompileResult r = compileXt(body);
    REQUIRE(r.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 200});
    doc->update(0.0f);
    const SceneInstance* inst = doc->sceneInstance(doc->byId("S"));
    REQUIRE(inst != nullptr);
    const NodeArena& arena = doc->arena();
    NodeId path = kInvalidNode, root = kInvalidNode;
    for (NodeId id : inst->localToArena) {
        const std::string_view name = arena.name(id) != kInvalidString ? doc->string(arena.name(id)) : "";
        if (name == "P") path = id;
        if (name == "Root") root = id;
    }
    REQUIRE(path != kInvalidNode);
    REQUIRE(root != kInvalidNode);
    CHECK(getProperty(arena, root, prop::Rotation).f == doctest::Approx(90.0f));
    // inverseBind = inverse(rest T(10, 20)).
    const SkinData& skin = arena.path(path).skin;
    REQUIRE(skin.tendons.size() == 1);
    CHECK(skin.tendons[0].inverseBind.a == doctest::Approx(1.0f));
    CHECK(skin.tendons[0].inverseBind.b == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(skin.tendons[0].inverseBind.e == doctest::Approx(-10.0f));
    CHECK(skin.tendons[0].inverseBind.f == doctest::Approx(-20.0f));
    // Skinned = world(pose) x inverse(rest) x own: (x, y) -> (10 - (y - 20), 20 + (x - 10)).
    const SkinnedGeometry* g = arena.skinned(path);
    REQUIRE(g != nullptr);
    const std::vector<float>& src = arena.path(path).points;
    REQUIRE(g->points.size() == src.size());
    for (size_t i = 0; i + 1 < src.size(); i += 2) {
        CAPTURE(i);
        CHECK(g->points[i] == doctest::Approx(10.0f - (src[i + 1] - 20.0f)).epsilon(1e-3));
        CHECK(g->points[i + 1] == doctest::Approx(20.0f + (src[i] - 10.0f)).epsilon(1e-3));
    }
}

TEST_CASE("rig: rest-* defaults to the pose; a child's rest-translate is the parent rest tip (T-249)") {
    // Without rest-*: the old inverse binds.
    auto plain = parse(std::string(R"X(<path id="P" d="M0 0 L1 0" xt-skin="#Rig">
        <xt-weights bones="Root Tip">0 0:1</xt-weights></path>)X") +
                       kSkeleton);
    auto same = parse(std::string(R"X(<path id="P" d="M0 0 L1 0" xt-skin="#Rig">
        <xt-weights bones="Root Tip">0 0:1</xt-weights></path>
        <xt-skeleton id="Rig">
          <xt-bone id="Root" length="50" translate="10 20" rotate="90" rest-length="50" rest-translate="10 20" rest-rotate="90" rest-scale="1">
            <xt-bone id="Tip" length="30" rest-rotate="0" rest-length="30"/>
          </xt-bone>
        </xt-skeleton>)X"));
    REQUIRE(plain->status == Status::kOk);
    REQUIRE(same->status == Status::kOk);
    REQUIRE(plain->scene.tendonPool.size() == 2);
    REQUIRE(same->scene.tendonPool.size() == 2);
    for (size_t t = 0; t < 2; ++t) {
        for (int k = 0; k < 6; ++k) {
            CHECK(plain->scene.tendonPool[t].inverseBind[k] == same->scene.tendonPool[t].inverseBind[k]);
        }
    }
    // The Tip rest sits at the Root's rest-length (80), not its length (50); a stray child
    // rest-translate warns and is ignored.
    auto longer = parse(std::string(R"X(<path id="P" d="M0 0 L1 0" xt-skin="#Rig">
        <xt-weights bones="Tip">0 0:1</xt-weights></path>
        <xt-skeleton id="Rig">
          <xt-bone id="Root" length="50" rest-length="80">
            <xt-bone id="Tip" length="30" rest-translate="5 5"/>
          </xt-bone>
        </xt-skeleton>)X"));
    REQUIRE(longer->status == Status::kOk);
    CHECK(longer->warned("rest-translate"));
    CHECK(longer->scene.tendonPool[0].inverseBind[4] == doctest::Approx(-80.0f));
    CHECK(longer->scene.tendonPool[0].inverseBind[5] == doctest::Approx(0.0f));
    // The pose keeps the plain length.
    CHECK(longer->scene.nodes[longer->node("Tip")].translate.x == doctest::Approx(50.0f));
}

TEST_CASE("rig: <xt-mesh x y> offsets the uvs; the default pins sit at (x, y) (T-249)") {
    auto p =
        parse(R"X(<xt-mesh id="M" href="a.png" x="10" y="20" width="100" height="50" transform="rotate(30)">
        <xt-pins>10,20 110,20 110,70 10,70 60,45</xt-pins></xt-mesh>
        <xt-mesh id="D" href="a.png" x="5" y="6" width="10" height="10"/>)X");
    REQUIRE(p->status == Status::kOk);
    REQUIRE(p->scene.meshes.size() == 2);
    const IrMeshData& m = p->scene.meshes[0];
    const Vec2* pos = p->scene.meshVec2Pool.data() + m.posOffset;
    const Vec2* uv = pos + m.vertexCount;
    CHECK(pos[2].x == doctest::Approx(110.0f));
    CHECK(uv[0].x == doctest::Approx(0.0f));
    CHECK(uv[0].y == doctest::Approx(0.0f));
    CHECK(uv[2].x == doctest::Approx(1.0f));
    CHECK(uv[2].y == doctest::Approx(1.0f));
    CHECK(uv[4].x == doctest::Approx(0.5f));
    CHECK(uv[4].y == doctest::Approx(0.5f));
    // The transform is the node's, as for any shape.
    CHECK(p->scene.nodes[p->node("M")].rotation == doctest::Approx(30.0f));
    const IrMeshData& d = p->scene.meshes[1];
    const Vec2* dpos = p->scene.meshVec2Pool.data() + d.posOffset;
    CHECK(dpos[0].x == doctest::Approx(5.0f));
    CHECK(dpos[2].y == doctest::Approx(16.0f));
    CHECK((dpos + d.vertexCount)[2].x == doctest::Approx(1.0f));
}

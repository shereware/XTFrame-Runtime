#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/value.h"

/// T-190: Path-typed tracks (docs/design/03-scene-and-properties.md §3.12.6), inline `<svg>` timeline
/// content (11-lottie-mapping.md §11.9), the clip clock (`clip-start`/`clip-rate`,
/// 04-timeline.md §4.10.2) and symbol-scoped `@timeline`s rewritten per instance (26 §26.2.1,
/// 04 §4.10.1). See docs/tasks/T-190.md.

using namespace xt;
using namespace xt::compile;

namespace {

CompileResult compileMain(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;
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

struct Loaded {
    CompileResult result;
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
    out.doc->setViewport(Rect{0, 0, 200, 100});
    out.doc->update(0.0f);
    return out;
}

/// The scene nodes of kind `kind` under element `id`, in document order.
std::vector<NodeId> sceneNodes(Document& d, std::string_view id, NodeKind kind) {
    std::vector<NodeId> out;
    ElementRef el = d.byId(id);
    REQUIRE(el.valid());
    d.arena().traversePreorder(el.node, [&](NodeId n) {
        if (d.arena().kind(n) == kind) {
            out.push_back(n);
        }
        return true;
    });
    return out;
}

}  // namespace

TEST_CASE("Path value: interpolates between equal topologies, jumps otherwise (03 3.12.6)") {
    const float a[] = {0, 0, 10, 0, 10, 10};
    const float b[] = {0, 0, 20, 0, 20, 20};
    float scratch[6] = {};
    Value mid = lerpValue(Value::ofPath(a, 6), Value::ofPath(b, 6), 0.5f, scratch, nullptr);
    CHECK(mid.floats[2] == doctest::Approx(15.0f));

    // Same point count, different verbs (M L L vs M L Z M): no interpolation.
    const float verbsA[] = {0, 1, 1};
    const float verbsB[] = {0, 1, 3, 0};
    Value va = Value::ofPathWithVerbs(a, 6, verbsA, 3);
    Value vb = Value::ofPathWithVerbs(b, 6, verbsB, 4);
    CHECK_FALSE(pathTopologyEquals(va, vb));
    Value early = lerpValue(va, vb, 0.4f, scratch, nullptr);
    CHECK(early.floats == a);
    Value late = lerpValue(va, vb, 0.6f, scratch, nullptr);
    CHECK(late.floats == b);
    CHECK(late.stopCount == 4);
    // Equal verbs interpolate.
    Value vc = Value::ofPathWithVerbs(b, 6, verbsA, 3);
    CHECK(pathTopologyEquals(va, vc));
    CHECK(lerpValue(va, vc, 0.5f, scratch, nullptr).floats[5] == doctest::Approx(15.0f));
}

TEST_CASE("inline <svg>: d, fill, path-trim and group transform keys drive the scene") {
    std::string xt = R"(<xt><head><style>
      #Icon { position: absolute; left: 0px; top: 0px; }
      @timeline main {
        duration: 1s;
        #Icon/p { 0s { d: path("M0 0 L10 0 L10 10 Z") } 1s { d: path("M0 0 L20 0 L20 20 Z") } }
        #Icon/p { 0s { fill: #ff0000 } 1s { fill: #0000ff } }
        #Icon/line { 0s { path-trim-end: 0 } 1s { path-trim-end: 1 } }
        #Icon/g { 0s { transform: rotate(0deg) } 1s ease-in { transform: rotate(90deg) } }
      }
    </style></head><body>
      <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
        <g id="g"><path id="p" d="M0 0 L10 0 L10 10 Z" fill="#ff0000"/></g>
        <path id="line" d="M0 16 L32 16" stroke="#000000" path-trim="0 0.5"/>
      </svg>
    </body></xt>)";
    Loaded l = load(xt);
    Document& d = *l.doc;
    std::vector<NodeId> paths = sceneNodes(d, "Icon", NodeKind::Path);
    REQUIRE(paths.size() == 2);
    // Static state before the timeline plays: the SVG's own values (path-trim attribute included).
    CHECK(d.arena().path(paths[1]).trimEnd == doctest::Approx(0.5f));
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    const PathNodeData& p = d.arena().path(paths[0]);
    REQUIRE(p.points.size() == 6);
    CHECK(p.points[2] == doctest::Approx(15.0f));
    CHECK(p.fill.color.r == doctest::Approx(0.5f));
    CHECK(p.fill.color.b == doctest::Approx(0.5f));
    CHECK(d.arena().path(paths[1]).trimEnd == doctest::Approx(0.5f));
    std::vector<NodeId> groups = sceneNodes(d, "Icon", NodeKind::Group);
    REQUIRE(groups.size() >= 2);  // The svg root, then #g.
    CHECK(d.arena().common(groups[1]).rotation > 0.0f);
    CHECK(d.arena().common(groups[1]).rotation < 45.0f);  // ease-in arriving at 1s.
    REQUIRE(d.seekTimeline("", "main", 1.0f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().common(groups[1]).rotation == doctest::Approx(90.0f));
    CHECK(d.arena().path(paths[1]).trimEnd == doctest::Approx(1.0f));
}

TEST_CASE("inline <svg>: a d key with another topology jumps (verbs travel with the keys)") {
    std::string xt = R"(<xt><head><style>
      @timeline main {
        #S/p { 0s { d: path("M0 0 L10 0 L10 10 Z") } 1s { d: path("M0 0 L20 0 L20 20 L0 20 Z") } }
      }
    </style></head><body>
      <svg id="S" viewBox="0 0 32 32"><path id="p" d="M0 0 L10 0 L10 10 Z" fill="#000000"/></svg>
    </body></xt>)";
    Loaded l = load(xt);
    Document& d = *l.doc;
    std::vector<NodeId> paths = sceneNodes(d, "S", NodeKind::Path);
    REQUIRE(paths.size() == 1);
    const size_t verbs0 = d.arena().path(paths[0]).verbs.size();
    REQUIRE(d.seekTimeline("", "main", 0.4f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().path(paths[0]).verbs.size() == verbs0);
    REQUIRE(d.seekTimeline("", "main", 0.6f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().path(paths[0]).verbs.size() == verbs0 + 1);
    CHECK(d.arena().path(paths[0]).points.size() == 8);
    REQUIRE(d.seekTimeline("", "main", 0.1f) == Status::kOk);  // And back.
    d.update(0.0f);
    CHECK(d.arena().path(paths[0]).verbs.size() == verbs0);
}

TEST_CASE("inline <svg> timeline errors and vector properties outside @timeline") {
    CompileResult unknown = compileMain(R"(<xt><head><style>
      @timeline main { #S/nope { 0s { fill: #000000 } 1s { fill: #ffffff } } }
    </style></head><body><svg id="S" viewBox="0 0 8 8"><path id="p" d="M0 0 L8 8"/></svg></body></xt>)");
    CHECK(hasDiagnostic(unknown, Diagnostic::Level::Error, "has no node \"nope\""));

    CompileResult onElement = compileMain(R"(<xt><head><style>
      @timeline main { #Box { 0s { fill: #000000 } 1s { fill: #ffffff } } }
    </style></head><body><div id="Box"></div></body></xt>)");
    CHECK(hasDiagnostic(onElement, Diagnostic::Level::Error, "is not inline <svg> content"));

    CompileResult notOnGroup = compileMain(R"(<xt><head><style>
      @timeline main { #S/g { 0s { fill: #000000 } 1s { fill: #ffffff } } }
    </style></head><body><svg id="S" viewBox="0 0 8 8"><g id="g"><path d="M0 0 L8 8"/></g></svg></body></xt>)");
    CHECK(hasDiagnostic(notOnGroup, Diagnostic::Level::Error, "does not apply to #S/g"));

    CompileResult inRule = compileMain(R"(<xt><head><style>
      #Box { fill: #ff0000; clip-rate: 2; }
    </style></head><body><div id="Box"></div></body></xt>)");
    CHECK(inRule.ok());
    CHECK(hasDiagnostic(inRule, Diagnostic::Level::Warning, "only applies to inline <svg> content"));
}

TEST_CASE("clip clock: clip-start / clip-rate place an <anim sync> on the group's time (04 4.10.2)") {
    std::string xt = R"(<xt><head><style>
      #clip { clip-start: 0.5s; clip-rate: 2; }
      @timeline t { duration: 3s; }
    </style></head><body><anim id="clip" src="min_solid_anim.json" sync="true"></anim></body></xt>)";
    Loaded l = load(xt);
    Document& d = *l.doc;
    uint32_t inst = d.defaultAnimation(d.byId("clip"));
    REQUIRE(inst != 0);
    REQUIRE(d.seekTimeline("", "t", 1.0f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.animationTime(inst) == doctest::Approx(1.0f));  // (1.0 - 0.5) * 2
    REQUIRE(d.seekTimeline("", "t", 0.25f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.animationTime(inst) == doctest::Approx(0.0f));  // Before the clip starts: clamped.
    REQUIRE(d.seekTimeline("", "t", 0.75f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.animationTime(inst) == doctest::Approx(0.5f));
}

TEST_CASE("symbol-scoped @timeline: one group per instance, a <use sync> follows the parent clock") {
    std::string xt = R"(<xt><head>
      <symbol name="Pulse"><div id="Dot"></div></symbol>
      <style>
        symbol(Pulse) {
          #Dot { width: 10px; height: 10px; }
          @timeline main { duration: 1s; #Dot { 0s { opacity: 0 } 1s { opacity: 1 } } }
        }
        #B { clip-start: 0.5s; clip-rate: 2; }
        @timeline main { duration: 2s; }
      </style></head>
      <body><use symbol="Pulse" id="A"/><use symbol="Pulse" id="B" sync="true"/></body></xt>)";
    Loaded l = load(xt);
    Document& d = *l.doc;

    // Three groups: the root's and one per instance, scoped to the instance root.
    const TimelineGroupChunk& chunk = d.ir().timelineGroups();
    REQUIRE(chunk.present());
    REQUIRE(chunk.groups().size() == 3);
    int scoped = 0;
    for (const IrTimelineGroup& g : chunk.groups()) {
        scoped += g.scopeElement != 0 ? 1 : 0;
    }
    CHECK(scoped == 2);

    // An instance's own timeline plays on its own.
    REQUIRE(d.playTimeline("A", "main") == Status::kOk);
    d.update(0.25f);
    CHECK(d.byId("A/Dot").property(prop::Opacity).f == doctest::Approx(0.25f));
    CHECK(d.byId("B/Dot").property(prop::Opacity).f == doctest::Approx(1.0f));  // Styled value.
    REQUIRE(d.stopTimeline("A", "main") == Status::kOk);

    // The root timeline drives only the sync instance, in its clip time.
    REQUIRE(d.seekTimeline("", "main", 0.75f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.byId("B/Dot").property(prop::Opacity).f == doctest::Approx(0.5f));  // (0.75 - 0.5) * 2
    CHECK(d.timelineTime("B", "main") == doctest::Approx(0.5f));
    CHECK(d.byId("A/Dot").property(prop::Opacity).f == doctest::Approx(1.0f));
    REQUIRE(d.stopTimeline("", "main") == Status::kOk);
    CHECK_FALSE(d.isTimelinePlaying("B", "main"));
}

TEST_CASE("symbol-scoped @timeline from .xtanim.json is rewritten per instance too") {
    const char* anim = R"({ "v": 1, "timelines": [ { "name": "Pulse", "duration": 1,
      "tracks": [ { "layer": "Dot", "prop": "opacity", "keys": [[0, 0], [1, 1]] } ] } ] })";
    std::string xt = R"(<xt><head>
      <symbol name="Pulse"><div id="Dot"></div></symbol>
      <style>symbol(Pulse) { @timeline main from "p.xtanim.json#Pulse"; }</style></head>
      <body><use symbol="Pulse" id="A"/></body></xt>)";
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}, {"p.xtanim.json", anim}};
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(r.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 100, 100});
    doc->update(0.0f);
    REQUIRE(doc->seekTimeline("A", "main", 0.5f) == Status::kOk);
    doc->update(0.0f);
    CHECK(doc->byId("A/Dot").property(prop::Opacity).f == doctest::Approx(0.5f));
    CHECK(doc->seekTimeline("", "main", 0.5f) == Status::kNotFound);
}

TEST_CASE("inline <svg>: viewBox keeps its case through the .xt tokenizer (no width/height needed)") {
    std::string xt = R"(<xt><head><style>#Icon { position: absolute; }</style></head><body>
      <svg id="Icon" viewBox="0 0 40 20"><path id="p" d="M0 0 L10 0 L10 10 Z" fill="#ff0000"/></svg>
    </body></xt>)";
    Loaded l = load(xt);
    for (const Diagnostic& d : l.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.message.find("no viewBox") == std::string::npos);
    }
    std::vector<NodeId> paths = sceneNodes(*l.doc, "Icon", NodeKind::Path);
    REQUIRE(paths.size() == 1);
}

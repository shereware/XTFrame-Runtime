#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/style_engine.h"
#include "xt/value.h"

/// T-218 (Docs/design/31-vector-authoring.md §31.5.1): vector properties in the style cascade --
/// scene-target rules (`#Icon/Blade`, `#Icon/*`, `#Icon/path`) fed by tokens, `:hover`, `:state()`,
/// `transition`, and losing to a `@timeline` on the same property. See Docs/tasks/T-218.md.

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

NodeId node(Document& d, std::string_view svgId, std::string_view inner) {
    ElementRef el = d.byId(svgId);
    REQUIRE(el.valid());
    NodeId n = el.sceneNode(inner);
    REQUIRE(n != kInvalidNode);
    return n;
}

Color fillOf(Document& d, std::string_view svgId, std::string_view inner) {
    const NodeId n = node(d, svgId, inner);
    // T-225b: `<rect>` is a Shape node.
    return d.arena().kind(n) == NodeKind::Shape ? d.arena().shape(n).fill.color
                                                : d.arena().path(n).fill.color;
}

void pointerTo(Document& d, float x, float y, float clock) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = Vec2{x, y};
    d.dispatchInput(ev);
    d.update(clock);
}

const char* kIcon = R"(
  <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
    <g id="G"><path id="Blade" d="M0 0 L10 0 L10 10 Z" fill="#0000ff"/>
      <rect id="R" x="12" y="0" width="8" height="8" fill="#000000"/></g>
    <path id="Line" d="M0 20 L32 20" stroke="#000000"/>
  </svg>)";

}  // namespace

TEST_CASE("scene-target rule: a token feeds the fill, a theme change re-applies it") {
    Loaded l = load(std::string(R"(<xt><head><style>
      :root { --accent: #00ff00; }
      #Icon/Blade { fill: var(--accent); }
    </style></head><body>)") +
                    kIcon + "</body></xt>");
    Document& d = *l.doc;
    Color c = fillOf(d, "Icon", "Blade");
    CHECK(c.r == doctest::Approx(0.0f));
    CHECK(c.g == doctest::Approx(1.0f));
    CHECK(c.b == doctest::Approx(0.0f));

    TokenMap theme;
    TokenValue accent;
    accent.value = Value::ofColor(Color{1, 0, 1, 1});
    accent.hasValue = true;
    theme["--accent"] = accent;
    d.setTokenLayers(&theme, nullptr);
    d.update(0.0f);
    c = fillOf(d, "Icon", "Blade");
    CHECK(c.r == doctest::Approx(1.0f));
    CHECK(c.g == doctest::Approx(0.0f));
    CHECK(c.b == doctest::Approx(1.0f));
    // Only the named node: the rect keeps its own fill.
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(0.0f));
}

TEST_CASE("scene-target rule: ancestor :hover overrides, leaving restores the cascade / IR value") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Btn { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px; }
      #Icon/Blade { fill: #00ff00; }
      #Btn:hover #Icon/Blade { fill: #ff0000; }
      #Btn:hover #Icon/R { fill: #ffffff; }
    </style></head><body><div id="Btn">)") +
                    kIcon + "</div></body></xt>");
    Document& d = *l.doc;
    CHECK(fillOf(d, "Icon", "Blade").g == doctest::Approx(1.0f));
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(0.0f));

    pointerTo(d, 60, 25, 0.0f);
    REQUIRE(d.byId("Btn").hasState(kStateHover));
    CHECK(fillOf(d, "Icon", "Blade").r == doctest::Approx(1.0f));
    CHECK(fillOf(d, "Icon", "Blade").g == doctest::Approx(0.0f));
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(1.0f));

    pointerTo(d, 190, 90, 0.0f);
    REQUIRE_FALSE(d.byId("Btn").hasState(kStateHover));
    CHECK(fillOf(d, "Icon", "Blade").g == doctest::Approx(1.0f));  // Back to the plain rule.
    CHECK(fillOf(d, "Icon", "Blade").r == doctest::Approx(0.0f));
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(0.0f));  // Back to the <svg>'s own fill.
    CHECK(fillOf(d, "Icon", "R").g == doctest::Approx(0.0f));
}

TEST_CASE("scene-target rule: `*` and a tag match every / every such node; `d` replaces the path") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Icon/* { stroke-width: 3px; }
      #Icon/path { opacity: 0.5; }
      #Icon/Line { d: path("M0 0 L20 0 L20 20 L0 20 Z"); }
    </style></head><body>)") +
                    kIcon + "</body></xt>");
    Document& d = *l.doc;
    int wide = 0;
    int half = 0;
    int groupsTouched = 0;
    d.arena().traversePreorder(d.byId("Icon").node, [&](NodeId n) {
        if (d.arena().kind(n) == NodeKind::Path && d.arena().path(n).strokeStyle.width == 3.0f) {
            ++wide;
        }
        // T-225b: the <rect> is a Shape node.
        if (d.arena().kind(n) == NodeKind::Shape && d.arena().shape(n).strokeStyle.width == 3.0f) {
            ++wide;
        }
        if (d.arena().kind(n) != NodeKind::Element && d.arena().common(n).opacity == 0.5f) {
            ++half;
        }
        if (d.arena().kind(n) == NodeKind::Group && d.arena().common(n).opacity != 1.0f) {
            ++groupsTouched;
        }
        return true;
    });
    CHECK(wide == 3);  // Blade, the rect (a Shape) and Line (a group takes no stroke-width).
    CHECK(half == 2);  // Only the two <path> elements, not the <rect>.
    CHECK(groupsTouched == 0);
    const PathNodeData& line = d.arena().path(node(d, "Icon", "Line"));
    CHECK(line.points.size() == 8);
    CHECK(line.verbs.size() == 5);
}

TEST_CASE("scene-target rule: :state() on an ancestor and on the <svg> itself") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Hp:state(Low) #Icon/Blade { fill: #ff0000; }
      #Icon/R:state(Low) { fill: #ffffff; }
    </style></head><body><div id="Hp">)") +
                    kIcon + "</div></body></xt>");
    Document& d = *l.doc;
    CHECK(fillOf(d, "Icon", "Blade").b == doctest::Approx(1.0f));
    REQUIRE(d.setCustomState(d.byId("Hp"), "Low", true) == Status::kOk);
    d.update(0.0f);
    CHECK(fillOf(d, "Icon", "Blade").r == doctest::Approx(1.0f));
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(0.0f));  // The state is on #Hp, not #Icon.
    REQUIRE(d.setCustomState(d.byId("Icon"), "Low", true) == Status::kOk);
    d.update(0.0f);
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(1.0f));
    REQUIRE(d.setCustomState(d.byId("Hp"), "Low", false) == Status::kOk);
    d.update(0.0f);
    CHECK(fillOf(d, "Icon", "Blade").r == doctest::Approx(0.0f));
    CHECK(fillOf(d, "Icon", "Blade").b == doctest::Approx(1.0f));
}

TEST_CASE("scene-target rule: `transition: fill` runs on the scene node") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Btn { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px; }
      #Icon { transition: fill 100ms linear; }
      #Icon/R { fill: #000000; }
      #Btn:hover #Icon/R { fill: #ff0000; }
    </style></head><body><div id="Btn">)") +
                    kIcon + "</div></body></xt>");
    Document& d = *l.doc;
    pointerTo(d, 60, 25, 1.0f);
    d.update(1.05f);
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(0.5f).epsilon(0.05));
    d.update(1.2f);
    CHECK(fillOf(d, "Icon", "R").r == doctest::Approx(1.0f));
}

TEST_CASE("scene-target rule: an @timeline on the same property wins") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Icon/Blade { fill: #00ff00; }
      @timeline main { #Icon/Blade { 0s { fill: #000000 } 1s { fill: #0000ff } } }
    </style></head><body>)") +
                    kIcon + "</body></xt>");
    Document& d = *l.doc;
    CHECK(fillOf(d, "Icon", "Blade").g == doctest::Approx(1.0f));  // Not playing: the rule.
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    Color c = fillOf(d, "Icon", "Blade");
    CHECK(c.g == doctest::Approx(0.0f));
    CHECK(c.b == doctest::Approx(0.5f));
}

TEST_CASE("scene-target rule diagnostics") {
    CompileResult unknown = compileMain(std::string(R"(<xt><head><style>
      #Icon/Nope { fill: #ff0000; }
    </style></head><body>)") + kIcon + "</body></xt>");
    CHECK(unknown.ok());
    CHECK(hasDiagnostic(unknown, Diagnostic::Level::Warning, "vector.unknownTarget"));

    CompileResult nonVector = compileMain(std::string(R"(<xt><head><style>
      #Icon/Blade { fill: #ff0000; width: 10px; }
      #Icon/R { fill: url(#grad); }
    </style></head><body>)") + kIcon + "</body></xt>");
    CHECK(nonVector.ok());
    CHECK(hasDiagnostic(nonVector, Diagnostic::Level::Warning,
                        "only vector properties apply to <svg> content"));
    CHECK(hasDiagnostic(nonVector, Diagnostic::Level::Warning, "url()"));

    CompileResult notSubject = compileMain(std::string(R"(<xt><head><style>
      #Icon/* #X { fill: #ff0000; }
    </style></head><body>)") + kIcon + "</body></xt>");
    CHECK(hasDiagnostic(notSubject, Diagnostic::Level::Error, "must be the selector's last compound"));
}

// ---------------------------------------------------------------------------------------------
// T-236: stroke-dasharray in the cascade, the id-before-tag rule, the Vec2 modifier parameters and
// a single-path <xt-trim> demoted to the path's own trim.
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<float> dashesOf(Document& d, std::string_view svgId, std::string_view inner) {
    const NodeId n = node(d, svgId, inner);
    return d.arena().kind(n) == NodeKind::Shape ? d.arena().shape(n).dashes : d.arena().path(n).dashes;
}

const char* kRing = R"(
  <svg id="Ring" viewBox="0 0 32 32" width="32" height="32">
    <circle id="C" cx="16" cy="16" r="10" fill="none" stroke="#000000"/>
    <path id="P" d="M0 0 L20 0" stroke="#000000" stroke-dasharray="3 3"/>
  </svg>)";

}  // namespace

TEST_CASE("scene-target rule: stroke-dasharray, :hover swaps the array, none and back to the IR") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Btn { position: absolute; left: 0px; top: 0px; width: 100px; height: 50px; }
      #Ring/C { stroke-dasharray: 6 4; }
      #Btn:hover #Ring/C { stroke-dasharray: 2px, 2px, 1px; }
      #Btn:hover #Ring/P { stroke-dasharray: none; }
    </style></head><body><div id="Btn">)") +
                    kRing + "</div></body></xt>");
    Document& d = *l.doc;
    CHECK(dashesOf(d, "Ring", "C") == std::vector<float>{6, 4});
    CHECK(dashesOf(d, "Ring", "P") == std::vector<float>{3, 3});

    pointerTo(d, 60, 25, 0.0f);
    REQUIRE(d.byId("Btn").hasState(kStateHover));
    CHECK(dashesOf(d, "Ring", "C") == std::vector<float>{2, 2, 1});
    CHECK(dashesOf(d, "Ring", "P").empty());

    pointerTo(d, 190, 90, 0.0f);
    CHECK(dashesOf(d, "Ring", "C") == std::vector<float>{6, 4});
    CHECK(dashesOf(d, "Ring", "P") == std::vector<float>{3, 3});  // The <svg>'s own array.
}

TEST_CASE("@timeline: stroke-dasharray keys hold; a transition on it is refused") {
    Loaded l = load(std::string(R"(<xt><head><style>
      @timeline main { #Ring/C { 0s { stroke-dasharray: 1 1 } 1s { stroke-dasharray: 5 5 } } }
    </style></head><body>)") +
                    kRing + "</body></xt>");
    Document& d = *l.doc;
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(dashesOf(d, "Ring", "C") == std::vector<float>{1, 1});
    REQUIRE(d.seekTimeline("", "main", 1.0f) == Status::kOk);
    d.update(0.0f);
    CHECK(dashesOf(d, "Ring", "C") == std::vector<float>{5, 5});

    CompileResult transition = compileMain(std::string(R"(<xt><head><style>
      #Ring { transition: stroke-dasharray 100ms linear; }
    </style></head><body>)") + kRing + "</body></xt>");
    CHECK(transition.ok());
    CHECK(hasDiagnostic(transition, Diagnostic::Level::Warning, "stroke-dasharray"));
}

TEST_CASE("scene target: an id wins over the tag name it equals, in rules and @timeline") {
    const char* withId = R"(
      <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
        <g id="g"><path id="A" d="M0 0 L10 0 L10 10 Z"/></g>
        <g><path id="B" d="M0 0 L10 0 L10 10 Z"/></g>
      </svg>)";
    const char* withoutId = R"(
      <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
        <g id="G1"><path id="A" d="M0 0 L10 0 L10 10 Z"/></g>
        <g><path id="B" d="M0 0 L10 0 L10 10 Z"/></g>
      </svg>)";
    auto halfGroups = [](Document& d) {
        int count = 0;
        d.arena().traversePreorder(d.byId("Icon").node, [&](NodeId n) {
            if (d.arena().kind(n) == NodeKind::Group && d.arena().common(n).opacity == 0.5f) {
                ++count;
            }
            return true;
        });
        return count;
    };
    {
        Loaded l = load(std::string("<xt><head><style>#Icon/g { opacity: 0.5; }</style></head><body>") +
                        withId + "</body></xt>");
        CHECK(halfGroups(*l.doc) == 1);
        CHECK(l.doc->arena().common(node(*l.doc, "Icon", "g")).opacity == 0.5f);
    }
    {
        Loaded l = load(std::string("<xt><head><style>#Icon/g { opacity: 0.5; }</style></head><body>") +
                        withoutId + "</body></xt>");
        CHECK(halfGroups(*l.doc) == 2);
    }
    {
        Loaded l = load(std::string(R"(<xt><head><style>
          @timeline main { #Icon/g { 0s { opacity: 0.5 } 1s { opacity: 0.5 } } }
        </style></head><body>)") +
                        withId + "</body></xt>");
        REQUIRE(l.doc->seekTimeline("", "main", 0.5f) == Status::kOk);
        l.doc->update(0.0f);
        CHECK(halfGroups(*l.doc) == 1);
    }
    {
        Loaded l = load(std::string(R"(<xt><head><style>
          @timeline main { #Icon/g { 0s { opacity: 0.5 } 1s { opacity: 0.5 } } }
        </style></head><body>)") +
                        withoutId + "</body></xt>");
        REQUIRE(l.doc->seekTimeline("", "main", 0.5f) == Status::kOk);
        l.doc->update(0.0f);
        CHECK(halfGroups(*l.doc) == 2);
    }
}

TEST_CASE("Vec2 modifier parameters: repeat-translate / repeat-anchor / center in rules and @timeline") {
    Loaded l = load(std::string(R"(<xt><head><style>
      #Rep/Tw { center: 3 4px; }
      #Rep/Rp2 { repeat-anchor: 1px 2px; }
      @timeline main { #Rep/Rp { 0s { repeat-translate: 0 0 } 1s { repeat-translate: 10px 20px } } }
    </style></head><body>
      <svg id="Rep" viewBox="0 0 32 32" width="32" height="32">
        <g><path id="P" d="M0 0 L4 0 L4 4 Z" fill="#000000"/><xt-repeat id="Rp" copies="3"/></g>
        <g><path id="Q" d="M0 0 L4 0 L4 4 Z" fill="#000000"/><xt-twist id="Tw" angle="10"/>
           <xt-repeat id="Rp2" copies="2"/></g>
      </svg></body></xt>)"));
    Document& d = *l.doc;
    const ModifierNodeData& tw = d.arena().modifier(node(d, "Rep", "Tw"));
    CHECK(tw.params[1] == doctest::Approx(3.0f));
    CHECK(tw.params[2] == doctest::Approx(4.0f));
    const ModifierNodeData& rp2 = d.arena().modifier(node(d, "Rep", "Rp2"));
    CHECK(rp2.params[2] == doctest::Approx(1.0f));
    CHECK(rp2.params[3] == doctest::Approx(2.0f));

    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    const ModifierNodeData& rp = d.arena().modifier(node(d, "Rep", "Rp"));
    CHECK(rp.params[4] == doctest::Approx(5.0f));
    CHECK(rp.params[5] == doctest::Approx(10.0f));
}

TEST_CASE("single-path <xt-trim>: its trim-* tracks drive the path's own trim") {
    Loaded l = load(std::string(R"(<xt><head><style>
      @timeline main { #Tr/Trim1 { 0s { trim-end: 0; trim-offset: 0 } 1s { trim-end: 1; trim-offset: 180 } } }
    </style></head><body>
      <svg id="Tr" viewBox="0 0 32 32" width="32" height="32">
        <path id="P" d="M0 0 L20 0" stroke="#000000"/><xt-trim id="Trim1" trim-end="0.5"/>
      </svg></body></xt>)"));
    Document& d = *l.doc;
    const NodeId p = node(d, "Tr", "P");
    CHECK(d.arena().path(p).trimEnd == doctest::Approx(0.5f));
    int modifiers = 0;
    d.arena().traversePreorder(d.byId("Tr").node, [&](NodeId n) {
        modifiers += d.arena().kind(n) == NodeKind::Modifier ? 1 : 0;
        return true;
    });
    CHECK(modifiers == 0);
    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().path(p).trimEnd == doctest::Approx(0.5f));
    CHECK(d.arena().path(p).trimOffset == doctest::Approx(0.25f));
    REQUIRE(d.seekTimeline("", "main", 1.0f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().path(p).trimEnd == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------------------------
// T-233 (31 §31.4.7): stroke taper and width profile.
// ---------------------------------------------------------------------------------------------

TEST_CASE("T-233: xt-taper / xt-width-profile reach the node; stroke-taper-* in @timeline and rules") {
    const char* svg = R"(
      <svg id="Stk" viewBox="0 0 32 32" width="32" height="32">
        <path id="P" d="M0 0 L20 0" stroke="#000000" stroke-width="4" xt-taper="0.1 0.2"
              xt-width-profile="0:1 0.5:2 1:1"/>
        <circle id="C" cx="16" cy="16" r="10" fill="none" stroke="#000000"/>
      </svg>)";
    Loaded l = load(std::string(R"(<xt><head><style>
      #Stk/C { stroke-taper-end: 0.3; }
      @timeline main { #Stk/P { 0s { stroke-taper-start: 0 } 1s { stroke-taper-start: 0.4 } } }
    </style></head><body>)") +
                    svg + "</body></xt>");
    Document& d = *l.doc;
    const NodeId p = node(d, "Stk", "P");
    const PathNodeData& pd = d.arena().path(p);
    CHECK(pd.strokeStyle.taperStart == doctest::Approx(0.1f));
    CHECK(pd.strokeStyle.taperEnd == doctest::Approx(0.2f));
    REQUIRE(pd.strokeStyle.profileCount == 3);
    REQUIRE(pd.widthProfile.size() == 3);
    CHECK(pd.widthProfile[1].t == doctest::Approx(0.5f));
    CHECK(pd.widthProfile[1].w == doctest::Approx(2.0f));
    const NodeId c = node(d, "Stk", "C");
    CHECK(d.arena().shape(c).strokeStyle.taperEnd == doctest::Approx(0.3f));

    REQUIRE(d.seekTimeline("", "main", 0.5f) == Status::kOk);
    d.update(0.0f);
    CHECK(d.arena().path(p).strokeStyle.taperStart == doctest::Approx(0.2f));
}

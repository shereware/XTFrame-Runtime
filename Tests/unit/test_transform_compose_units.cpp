#include <memory>
#include <string>
#include <string_view>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-869 B: a `transform` sequence that needs 05.5's matrix composition may hold `%` / `em` / `rem`
/// translates. They stay out of the matrix and land in the translate component in their own unit,
/// resolved at run time against the element's border box or font size.

using namespace xt;
using namespace xt::compile;

namespace {

struct Loaded {
    NullTextShaper shaper;
    std::unique_ptr<FileAssetResolver> assets;
    std::unique_ptr<Document> doc;
};

CompileResult compileText(std::string_view source) {
    SourceFile files[1] = {SourceFile{"compose.xt", source}};
    CompileOptions options;
    return compileDocumentFromStrings(files, "compose.xt", options);
}

void load(std::string_view source, Loaded& out) {
    CompileResult result = compileText(source);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    out.assets = std::make_unique<FileAssetResolver>(XT_TEST_LAYOUT_DIR, nullptr);
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &out.shaper, out.assets.get(),
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 800, 600});
    out.doc->update(0.0f);
}

std::string page(std::string_view boxStyle, std::string_view extra = {}) {
    std::string s =
        "<xt>\n  <head>\n    <style>\n      #b { position: absolute; left: 300px; top: 200px; "
        "width: 100px; height: 40px; font-size: 20px; ";
    s += boxStyle;
    s += " }\n      ";
    s += extra;
    s += "\n    </style>\n  </head>\n  <body><div id=\"b\"></div></body>\n</xt>\n";
    return s;
}

/// The four border-box corners of `#b` on screen, compared between two documents.
void checkSameCorners(Loaded& a, Loaded& b, float w, float h) {
    const NodeId na = a.doc->byId("b").node;
    const NodeId nb = b.doc->byId("b").node;
    const Vec2 corners[4] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    for (const Vec2& c : corners) {
        const Vec2 pa = a.doc->nodeToScreen(na, c);
        const Vec2 pb = b.doc->nodeToScreen(nb, c);
        CAPTURE(c.x);
        CAPTURE(c.y);
        CHECK(pa.x == doctest::Approx(pb.x).epsilon(1e-4));
        CHECK(pa.y == doctest::Approx(pb.y).epsilon(1e-4));
    }
}

bool hasError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("T-869 B: a percent translate first in a composed sequence matches the px version") {
    Loaded pct;
    load(page("transform: translate(-50%, -50%) scale(3) rotate(-14deg);", "#b.wide { width: 200px; }"), pct);
    Loaded px;
    load(page("transform: translate(-50px, -20px) scale(3) rotate(-14deg);"), px);
    checkSameCorners(pct, px, 100, 40);

    // The element grows: the percent follows its border box.
    REQUIRE(pct.doc->byId("b").addClass("wide"));
    pct.doc->update(0.0f);
    Loaded px2;
    load(page("width: 200px; transform: translate(-100px, -20px) scale(3) rotate(-14deg);"), px2);
    checkSameCorners(pct, px2, 200, 40);
}

TEST_CASE("T-869 B: a percent translate after a scale is scaled with it") {
    Loaded pct;
    load(page("transform: scale(2) translateX(-10%) rotate(30deg);"), pct);
    Loaded px;
    load(page("transform: scale(2) translateX(-10px) rotate(30deg);"), px);
    checkSameCorners(pct, px, 100, 40);
}

TEST_CASE("T-869 B: an em translate in a composed sequence uses the element's font size") {
    Loaded em;
    load(page("transform: translate(1em, 0.5em) scale(1.5) rotate(10deg);"), em);
    Loaded px;
    load(page("transform: translate(20px, 10px) scale(1.5) rotate(10deg);"), px);
    checkSameCorners(em, px, 100, 40);
}

TEST_CASE("T-869 B: keyframes of composed sequences with percent translates animate like px keys") {
    const char* pctKeys =
        "@keyframes k { 0% { transform: translate(-50%, -50%) scale(3) rotate(-14deg) } "
        "100% { transform: translate(-50%, -50%) scale(1) rotate(-6deg) } }";
    const char* pxKeys =
        "@keyframes k { 0% { transform: translate(-50px, -20px) scale(3) rotate(-14deg) } "
        "100% { transform: translate(-50px, -20px) scale(1) rotate(-6deg) } }";
    Loaded pct;
    load(page("animation: k 1s linear both;", pctKeys), pct);
    Loaded px;
    load(page("animation: k 1s linear both;", pxKeys), px);
    const float samples[] = {0.0f, 0.3f, 0.5f, 1.0f};
    for (float t : samples) {
        CAPTURE(t);
        pct.doc->update(t);
        px.doc->update(t);
        checkSameCorners(pct, px, 100, 40);
    }
}

TEST_CASE("T-869 B: a percent key next to `none` (a zero px translate) animates like px keys") {
    Loaded pct;
    load(page("animation: k 1s linear both;",
              "@keyframes k { from { transform: translate(-50%, -50%) scale(0.5) rotate(20deg) } "
              "to { transform: none } }"),
         pct);
    Loaded px;
    load(page("animation: k 1s linear both;",
              "@keyframes k { from { transform: translate(-50px, -20px) scale(0.5) rotate(20deg) } "
              "to { transform: none } }"),
         px);
    const float samples[] = {0.0f, 0.5f, 1.0f};
    for (float t : samples) {
        CAPTURE(t);
        pct.doc->update(t);
        px.doc->update(t);
        checkSameCorners(pct, px, 100, 40);
    }
}

TEST_CASE("T-869 B: what the components cannot hold is still a compile error") {
    // A percent translate after a skew depends on the width in y: no component holds that.
    CHECK(
        hasError(compileText(page("transform: skewY(-6deg) translateX(-110%);")), "after a rotate or skew"));
    // px and % left on one axis.
    CHECK(hasError(compileText(page("transform: translateX(10px) scale(2) translateX(10%);")),
                   "px and % / em together"));
}

#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"

/// T-594 (Docs/design/54-source-format-v2.md §54.14 S4, 24 §24.15.4): the `web.*` warnings of
/// CompileOptions::webCompat -- one case per code, each with its fix and its not-like-web.md
/// pointer, plus "off by default" and "a strict compile does not fail on them".

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileWeb(const std::string& xt, bool webCompat = true, bool strict = false) {
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileOptions options;
    options.webCompat = webCompat;
    options.strict = strict;
    return compileDocumentFromStrings(files, "main.xt", options);
}

std::vector<Diagnostic> withCode(const CompileResult& r, std::string_view code) {
    std::vector<Diagnostic> out;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.code == code) {
            out.push_back(d);
        }
    }
    return out;
}

size_t webCount(const CompileResult& r) {
    size_t n = 0;
    for (const Diagnostic& d : r.diagnostics) {
        n += d.code.rfind("web.", 0) == 0 ? 1u : 0u;
    }
    return n;
}

/// Exactly one `code` warning, with a fix and the not-like-web.md pointer; returns it.
Diagnostic single(const CompileResult& r, std::string_view code) {
    const std::vector<Diagnostic> found = withCode(r, code);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
    }
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK_FALSE(found[0].fix.empty());
    CHECK(found[0].message.find("(not-like-web.md: \"") != std::string::npos);
    return found[0];
}

}  // namespace

TEST_CASE("T-594: web.inlineBlock -- display: inline-block, inline, contents") {
    const CompileResult r = compileWeb(page("#A { display: inline-block; }", "<div id=\"A\"></div>"));
    REQUIRE(r.ok());
    const Diagnostic d = single(r, "web.inlineBlock");
    CHECK(d.message.find("display: inline-block") != std::string::npos);
    CHECK(d.fix.find("display: flex on the parent") != std::string::npos);
    const CompileResult flex = compileWeb(page("#A { display: inline-flex; }", "<div id=\"A\"></div>"));
    CHECK(single(flex, "web.inlineBlock").fix == "display: flex");
    CHECK(withCode(compileWeb(page("#A { display: contents; }", "<div id=\"A\"></div>")), "web.inlineBlock")
              .size() == 1);
    CHECK(webCount(compileWeb(page("#A { display: flex; }", "<div id=\"A\"></div>"))) == 0);
}

TEST_CASE("T-594: web.positionFixed -- position: fixed and sticky") {
    const CompileResult r = compileWeb(
        page("#A { position: fixed; } #B { position: sticky; }", "<div id=\"A\"></div><div id=\"B\"></div>"));
    REQUIRE(r.ok());
    CHECK(withCode(r, "web.positionFixed").size() == 2);
    CHECK(withCode(r, "web.positionFixed")[0].fix.find("position: absolute") != std::string::npos);
    CHECK(webCount(compileWeb(page("#A { position: absolute; }", "<div id=\"A\"></div>"))) == 0);
}

TEST_CASE("T-594: web.floatOrTable -- float, display: table and <table> markup") {
    const CompileResult r = compileWeb(page("#A { float: left; } #B { display: table-cell; }",
                                            "<div id=\"A\"></div><div id=\"B\"></div>"
                                            "<table id=\"T\"><tr><td>a</td></tr></table>"));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found = withCode(r, "web.floatOrTable");
    REQUIRE(found.size() == 3);  // the <table> (not its rows and cells), float, display: table-cell
    CHECK(found[0].message.find("<table>") == 0);
    CHECK(found[1].message.find("float: left") == 0);
    CHECK(found[2].message.find("display: table-cell") == 0);
    CHECK(webCount(compileWeb(page("#A { float: none; }", "<div id=\"A\"></div>"))) == 0);
}

TEST_CASE("T-594: web.boxSizing -- box-sizing other than border-box") {
    const CompileResult r = compileWeb(page("#A { box-sizing: content-box; }", "<div id=\"A\"></div>"));
    REQUIRE(r.ok());
    CHECK(single(r, "web.boxSizing").message.find("always border-box") != std::string::npos);
    CHECK(webCount(compileWeb(page("#A { box-sizing: border-box; }", "<div id=\"A\"></div>"))) == 0);
}

TEST_CASE("T-594: web.vwVhVsUiscale -- vw / vh lengths, in a style sheet and in a style attribute") {
    const CompileResult r = compileWeb(page("#A { width: 50vw; }",
                                            "<div id=\"A\"></div><div id=\"B\" "
                                            "style=\"height: 10vh\"></div>"));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found = withCode(r, "web.vwVhVsUiscale");
    REQUIRE(found.size() == 2);
    CHECK(found[0].message.find("width: 50vw") != std::string::npos);
    CHECK(found[1].message.find("height: 10vh") != std::string::npos);
    CHECK(webCount(compileWeb(page("#A { width: 50px; }", "<div id=\"A\"></div>"))) == 0);
}

TEST_CASE("T-594: web.textWithChildren -- an id / bind: on a span folded into rich text") {
    const CompileResult r = compileWeb(page("",
                                            "<div id=\"Line\" port:score=\"number in\">Score <span "
                                            "id=\"Value\" bind:text=\"score\">0</span></div>"));
    const Diagnostic d = single(r, "web.textWithChildren");
    CHECK(d.message.find("<span id=\"Value\">") != std::string::npos);
    CHECK(d.fix.find("in its own <span>") != std::string::npos);
    CHECK(d.line == 1);
    // Plain inline markup (no id, bind or on) is rich text as intended.
    CHECK(webCount(compileWeb(page("", "<div id=\"Line\">Score <b>bold</b></div>"))) == 0);
}

TEST_CASE("T-594: web.layerAttribute -- layer written as an attribute") {
    const CompileResult r = compileWeb(page("", "<div id=\"Confirm\" layer=\"overlay\"></div>"));
    REQUIRE(r.ok());
    CHECK(single(r, "web.layerAttribute").fix == "#Confirm { layer: overlay; }");
    CHECK(webCount(compileWeb(page("#Confirm { layer: overlay; }", "<div id=\"Confirm\"></div>"))) == 0);
}

TEST_CASE("T-594: web.zIndexAcrossLayers -- z-index inside a clipping parent or without siblings") {
    const CompileResult r =
        compileWeb(page("#Clip { overflow: hidden; width: 100px; height: 100px; } #Pop { z-index: 10; } "
                        "#Only { z-index: 2; } #Ok { z-index: 1; }",
                        "<div id=\"Clip\"><div id=\"Pop\"></div><div id=\"Q\"></div></div>"
                        "<div id=\"W\"><div id=\"Only\"></div></div><div id=\"Ok\"></div>"));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found = withCode(r, "web.zIndexAcrossLayers");
    REQUIRE(found.size() == 2);
    CHECK(found[0].message.find("#Pop") == 0);
    CHECK(found[0].message.find("clipped by #Clip") != std::string::npos);
    CHECK(found[0].fix == "#Pop { layer: overlay; }");
    CHECK(found[1].message.find("#Only") == 0);
    // layer: overlay is the cure.
    CHECK(webCount(compileWeb(page("#Clip { overflow: hidden; } #Pop { z-index: 10; layer: overlay; }",
                                   "<div id=\"Clip\"><div id=\"Pop\"></div><div id=\"Q\"></div></div>"))) ==
          0);
}

TEST_CASE("T-594: web.pointerEventsInherit -- a clickable element under pointer-events: none") {
    const CompileResult r =
        compileWeb(page("#Dialogs { pointer-events: none; } #Panel { pointer-events: auto; }",
                        "<div id=\"Dialogs\"><div id=\"Close\" on:click=\"emit(close)\"></div>"
                        "<div id=\"Panel\"><div id=\"Ok\" on:click=\"emit(ok)\"></div></div></div>"));
    REQUIRE(r.ok());
    const Diagnostic d = single(r, "web.pointerEventsInherit");
    CHECK(d.message.find("#Close takes no clicks") == 0);
    CHECK(d.message.find("#Dialogs sets none") != std::string::npos);
    CHECK(d.fix == "#Close { pointer-events: auto; }");
}

TEST_CASE("T-594: web.flexTextNoMinWidth -- a text flex item with no min-width") {
    const CompileResult r =
        compileWeb(page("#Row { display: flex; } #Fixed { min-width: 40px; } #Rigid { flex-shrink: 0; }",
                        "<div id=\"Row\"><span id=\"Label\">Long label</span><span id=\"Fixed\">a</span>"
                        "<span id=\"Rigid\">b</span></div>"));
    REQUIRE(r.ok());
    const Diagnostic d = single(r, "web.flexTextNoMinWidth");
    CHECK(d.message.find("#Label is a flex item with text") == 0);
    CHECK(d.fix.find("#Label { min-width:") == 0);
    // A column does not squeeze widths.
    CHECK(webCount(compileWeb(page("#Row { display: flex; flex-direction: column; }",
                                   "<div id=\"Row\"><span id=\"Label\">Long label</span></div>"))) == 0);
}

TEST_CASE("T-594: web.percentHeightOnAuto -- height in % of a parent whose height is auto") {
    const CompileResult r =
        compileWeb(page("#Box { width: 100px; } #Half { height: 50%; } "
                        "#Sized { height: 80px; } #Fine { height: 50%; }",
                        "<div id=\"Box\"><div id=\"Half\"></div></div>"
                        "<div id=\"Sized\"><div id=\"Fine\"></div></div>"));
    REQUIRE(r.ok());
    const Diagnostic d = single(r, "web.percentHeightOnAuto");
    CHECK(d.message.find("#Half has height 50% of #Box") == 0);
    CHECK(d.fix == "#Box { height: <height>; }");
}

TEST_CASE("T-594: web.translateZSort -- a 2D sibling between 3D siblings") {
    const CompileResult r =
        compileWeb(page("#A { transform: translateZ(20px); } #C { transform: rotateY(30deg); }",
                        "<div id=\"G\"><div id=\"A\"></div><div id=\"B\"></div>"
                        "<div id=\"C\"></div></div>"));
    REQUIRE(r.ok());
    const Diagnostic d = single(r, "web.translateZSort");
    CHECK(d.message.find("#B is a 2D layer between the 3D siblings #A and #C") == 0);
    CHECK(webCount(compileWeb(page("#A { transform: translateZ(20px); } #B { transform: translateZ(10px); }",
                                   "<div id=\"G\"><div id=\"A\"></div><div id=\"B\"></div></div>"))) == 0);
}

TEST_CASE("T-594: web.* is off by default and never fails a strict compile") {
    const std::string source = page("#A { display: inline-block; width: 10vw; } #Row { display: flex; }",
                                    "<div id=\"A\"></div><div id=\"Row\"><span id=\"T\">text</span></div>");
    const CompileResult off = compileWeb(source, false);
    REQUIRE(off.ok());
    CHECK(webCount(off) == 0);

    // Strict fails on the plain "invalid value" warning, as it always did ...
    CHECK_FALSE(compileWeb(source, true, true).ok());
    // ... but not on web.* warnings alone.
    const CompileResult strictWeb =
        compileWeb(page("#A { width: 10vw; } #Row { display: flex; }",
                        "<div id=\"A\"></div><div id=\"Row\"><span id=\"T\">text</span></div>"),
                   true, true);
    CHECK(strictWeb.ok());
    CHECK(webCount(strictWeb) == 2);
}

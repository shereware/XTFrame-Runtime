#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt_v2_test_util.h"

/// T-588 (Docs/design/54-source-format-v2.md §54.5.1): the anchor shorthands `anchor` / `offset` /
/// `offset-end` / `size` of a v2 `<layout>` section. Every expansion case compiles a document
/// written with the shorthand and the same document written with the longhands the table gives
/// (the `calc(1safe-...)` forms), and requires byte-identical IR; then the diagnostics.

using namespace xt::compile;
using xt::test::v2::compileFiles;
using xt::test::v2::findCode;
using xt::test::v2::requireCompiled;

namespace {

std::string v2Doc(const std::string& body, const std::string& layout, const std::string& extra) {
    return "<xt version=\"2\">\n<body>" + body + "</body>\n<layout>" + layout + "</layout>\n" + extra +
           "\n</xt>\n";
}

CompileResult compileLayout(const std::string& body, const std::string& layout,
                            const std::string& extra = {}) {
    return compileFiles({SourceFile{"anchor.xt", v2Doc(body, layout, extra)}});
}

/// `shorthand` and `longhand` (one line each, so every declaration keeps its line) give the same IR.
void checkSameIr(const std::string& body, const std::string& shorthand, const std::string& longhand,
                 const std::string& extra = {}) {
    CAPTURE(shorthand);
    const CompileResult a = compileLayout(body, shorthand, extra);
    const CompileResult b = compileLayout(body, longhand, extra);
    requireCompiled(a);
    requireCompiled(b);
    CHECK(a.ir == b.ir);
}

/// `layout` fails to compile with an error carrying `code` and a fix.
void checkError(const std::string& body, const std::string& layout, const std::string& code,
                const std::string& extra = {}) {
    CAPTURE(layout);
    const CompileResult r = compileLayout(body, layout, extra);
    CHECK_FALSE(r.ok());
    bool found = false;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.code == code) {
            found = true;
            CHECK_FALSE(d.fix.empty());
        }
    }
    CHECK(found);
}

const std::string kOne = "<div id=\"A\"></div>";
const std::string kNested = "<div id=\"P\"><div id=\"C\"></div><div id=\"D\"></div></div>";

}  // namespace

TEST_CASE("T-588 anchor: left / right per axis, safe and screen, zero short forms") {
    checkSameIr(kOne, "#A { anchor: left top; offset: 16px 26px; size: 342px 164px; }",
                "#A { position: absolute; left: calc(1safe-left + 16px); top: calc(1safe-top + 26px); width: "
                "342px; height: 164px; }");
    checkSameIr(kOne, "#A { anchor: left top; }",
                "#A { position: absolute; left: 1safe-left; top: 1safe-top; }");
    checkSameIr(
        kOne, "#A { anchor: right bottom safe; offset: 16px 6px; }",
        "#A { position: absolute; right: calc(1safe-right + 16px); bottom: calc(1safe-bottom + 6px); }");
    checkSameIr(kOne, "#A { anchor: right bottom screen; offset: 16px 0; }",
                "#A { position: absolute; right: 16px; bottom: 0; }");
    checkSameIr(kOne, "#A { anchor: left top screen; offset: 0px 0%; size: 10px 20px; }",
                "#A { position: absolute; left: 0; top: 0; width: 10px; height: 20px; }");
    checkSameIr(
        kOne, "#A { anchor: left bottom; offset: calc(2vw - 4px) -6px; }",
        "#A { position: absolute; left: calc(1safe-left + calc(2vw - 4px)); bottom: calc(1safe-bottom + "
        "-6px); }");
}

TEST_CASE("T-588 anchor: center is calc(50% - size / 2 + offset), stretch sets both edges") {
    checkSameIr(
        kOne, "#A { anchor: center center; offset: 0 10px; size: 320px 40px; }",
        "#A { position: absolute; left: calc(50% - 320px / 2); top: calc(50% - 40px / 2 + 10px); width: "
        "320px; height: 40px; }");
    checkSameIr(kOne, "#A { anchor: center top screen; offset: -12px 8px; size: 20vw auto; }",
                "#A { position: absolute; left: calc(50% - 20vw / 2 + -12px); top: 8px; width: 20vw; height: "
                "auto; }");
    // offset-end defaults to offset.
    checkSameIr(kOne, "#A { anchor: stretch stretch; offset: 8px 4px; }",
                "#A { position: absolute; left: calc(1safe-left + 8px); right: calc(1safe-right + 8px); top: "
                "calc(1safe-top + 4px); bottom: calc(1safe-bottom + 4px); }");
    checkSameIr(kOne,
                "#A { anchor: stretch top screen; offset: 8px 0; offset-end: 12px 0; size: auto 30px; }",
                "#A { position: absolute; left: 8px; right: 12px; top: 0; width: auto; height: 30px; }");
    checkSameIr(kOne, "#A { anchor: stretch stretch screen; }",
                "#A { position: absolute; left: 0; right: 0; top: 0; bottom: 0; }");
}

TEST_CASE("T-588 the longhands take the first shorthand's place, with its importance; size alone; var()") {
    checkSameIr(kOne, "#A { width: 5px; size: 10px auto; z-index: 1; }",
                "#A { width: 5px; width: 10px; height: auto; z-index: 1; }");
    checkSameIr(kOne, "#A { size: 10px 20px; }", "#A { width: 10px; height: 20px; }");
    checkSameIr(kOne, "#A { z-index: 2; anchor: left top !important; size: 4px 4px; }",
                "#A { z-index: 2; position: absolute !important; left: 1safe-left !important; top: 1safe-top "
                "!important; width: 4px; height: 4px; }");
    // A repeated shorthand: the last one wins.
    checkSameIr(kOne, "#A { anchor: right top; anchor: left top; }",
                "#A { position: absolute; left: 1safe-left; top: 1safe-top; }");
    checkSameIr(kOne, "#A { anchor: left top; offset: var(--gap) 0; }",
                "#A { position: absolute; left: calc(1safe-left + var(--gap)); top: 1safe-top; }",
                "<style>:root { --gap: 6px; }</style>");
}

TEST_CASE("T-588 the shorthands expand inside @media") {
    checkSameIr(
        kOne,
        "#A { anchor: left top; size: 40px 40px; } @media (aspect-ratio >= 21/9) { #A { anchor: right "
        "center; "
        "offset: 10px 0; size: 40px 40px; } }",
        "#A { position: absolute; left: 1safe-left; top: 1safe-top; width: 40px; height: 40px; } @media "
        "(aspect-ratio >= 21/9) { #A { position: absolute; right: calc(1safe-right + 10px); top: calc(50% - "
        "40px / 2); width: 40px; height: 40px; } }");
    checkSameIr(
        kNested,
        "#P { display: flex; } @media (max-width: 800px) { #C { size: fill auto; } #P { size: fill fill; } }",
        "#P { display: flex; } @media (max-width: 800px) { #C { flex-grow: 1; flex-basis: 0; height: auto; } "
        "#P { width: 100vw; height: 100vh; } }");
}

TEST_CASE("T-588 size: fill -- free child 100%, UI root 100vw/100vh, stack child flex-grow / align-self") {
    // Free child (the parent is a block).
    checkSameIr(kNested, "#P { width: 100px; height: 50px; } #C { size: fill fill; }",
                "#P { width: 100px; height: 50px; } #C { width: 100%; height: 100%; }");
    // UI root: a direct child of <body>.
    checkSameIr(kNested, "#P { size: fill fill; }", "#P { width: 100vw; height: 100vh; }");
    // Stack (row): x is the main axis, y the cross axis.
    checkSameIr(kNested, "#P { display: flex; } #C { size: fill fill; }",
                "#P { display: flex; } #C { flex-grow: 1; flex-basis: 0; align-self: stretch; }");
    // Stack (column, from another rule and the inline style): the axes swap.
    checkSameIr(
        "<div id=\"P\" class=\"col\"><div id=\"C\"></div></div>",
        "#P { display: flex; } .col { flex-direction: column; } #C { size: fill 20px; }",
        "#P { display: flex; } .col { flex-direction: column; } #C { align-self: stretch; height: 20px; }");
    checkSameIr("<div id=\"P\" style=\"display: flex\"><div id=\"C\"></div></div>", "#C { size: auto fill; }",
                "#C { width: auto; align-self: stretch; }");
    // Anchored or absolutely positioned: never a stack child.
    checkSameIr(
        kNested, "#P { display: flex; } #C { anchor: left top; size: fill 10px; }",
        "#P { display: flex; } #C { position: absolute; left: 1safe-left; top: 1safe-top; width: 100%; "
        "height: 10px; }");
    checkSameIr(kNested, "#P { display: flex; } #C { position: absolute; } #C { size: fill auto; }",
                "#P { display: flex; } #C { position: absolute; } #C { width: 100%; height: auto; }");
    // Importance carries over to the stack form.
    checkSameIr(kNested, "#P { display: flex; } #C { size: fill auto !important; }",
                "#P { display: flex; } #C { flex-grow: 1 !important; flex-basis: 0 !important; height: auto "
                "!important; }");
}

TEST_CASE("T-588 size: fill inside a symbol is resolved per instance after expansion") {
    const std::string symbol =
        "<symbol name=\"Row\"><div id=\"Box\"><div id=\"In\"></div></div><layout>#Box { display: "
        "flex; } #In { size: fill auto; }</layout></symbol>";
    const std::string symbolLong =
        "<symbol name=\"Row\"><div id=\"Box\"><div id=\"In\"></div></div><layout>#Box { "
        "display: flex; } #In { flex-grow: 1; flex-basis: 0; height: auto; }</layout></symbol>";
    const CompileResult a = compileLayout("<use symbol=\"Row\" id=\"R1\"/>", "#R1 { width: 10px; }", symbol);
    const CompileResult b =
        compileLayout("<use symbol=\"Row\" id=\"R1\"/>", "#R1 { width: 10px; }", symbolLong);
    requireCompiled(a);
    requireCompiled(b);
    CHECK(a.ir == b.ir);
}

TEST_CASE("T-588 size: fill is an error when the rule's elements need different forms") {
    const CompileResult r = compileLayout("<div id=\"P\" class=\"f\"><div id=\"C\" class=\"f\"></div></div>",
                                          "#P { display: flex; } .f { size: fill auto; }");
    CHECK_FALSE(r.ok());
    bool found = false;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find("size: fill") != std::string::npos) {
            found = true;
            CHECK_FALSE(d.fix.empty());
        }
    }
    CHECK(found);
}

TEST_CASE("T-588 layout.centerNeedsSize and layout.anchorConflict") {
    checkError(kOne, "#A { anchor: center top; }", "layout.centerNeedsSize");
    checkError(kOne, "#A { anchor: center top; size: auto 10px; }", "layout.centerNeedsSize");
    checkError(kOne, "#A { anchor: left center; size: 10px fill; }", "layout.centerNeedsSize");
    checkError(kOne, "#A { anchor: stretch top; size: 10px auto; }", "layout.anchorConflict");
    checkError(kOne, "#A { anchor: left stretch; size: 10px fill; }", "layout.anchorConflict");
    checkError(kOne, "#A { anchor: left top; left: 4px; }", "layout.anchorConflict");
    checkError(kOne, "#A { bottom: 4px; anchor: left top; }", "layout.anchorConflict");
    checkError(kOne, "#A { anchor: left top; position: relative; }", "layout.anchorConflict");
    checkError(kOne, "#A { anchor: left top; inset: 0; }", "layout.anchorConflict");
    // Another rule may still set the edges (the cascade decides).
    requireCompiled(compileLayout(kOne, "#A { anchor: left top; } #A { left: 4px; }"));
}

TEST_CASE("T-588 malformed shorthands are errors with a fix") {
    for (const char* layout :
         {"#A { anchor: left; }", "#A { anchor: top left; }", "#A { anchor: left top middle; }",
          "#A { anchor: 4px top; }", "#A { anchor: left top; offset: 4px; }",
          "#A { anchor: left top; size: 4px 4px 4px; }", "#A { offset: 4px 4px; }", "#A { size: 4px; }",
          "#A { anchor: left top; offset-end: 1px; }"}) {
        CAPTURE(layout);
        const CompileResult r = compileLayout(kOne, layout);
        CHECK_FALSE(r.ok());
        bool found = false;
        for (const Diagnostic& d : r.diagnostics) {
            found = found || (d.level == Diagnostic::Level::Error && !d.fix.empty());
        }
        CHECK(found);
    }
    // A bad value inside a component is reported (and dropped) by the longhand it lands in, exactly as
    // the hand-written `left: calc(1safe-left + red)` would be.
    const CompileResult bad = compileLayout(kOne, "#A { anchor: left top; offset: red 0; }");
    const CompileResult badLong =
        compileLayout(kOne, "#A { position: absolute; left: calc(1safe-left + red); top: 1safe-top; }");
    CHECK_FALSE(bad.diagnostics.empty());
    CHECK(bad.diagnostics.size() == badLong.diagnostics.size());
    CHECK(bad.ir == badLong.ir);
}

TEST_CASE("T-588 the shorthands outside <layout> are layout.notGeometry") {
    // v2 <style>.
    const CompileResult inStyle = compileFiles(
        {SourceFile{"s.xt",
                    "<xt version=\"2\"><body><div id=\"A\"></div></body><style>#A { anchor: left top; "
                    "}</style></xt>"}});
    CHECK_FALSE(inStyle.ok());
    const Diagnostic* d = findCode(inStyle.diagnostics, "layout.notGeometry");
    REQUIRE(d != nullptr);
    CHECK_FALSE(d->fix.empty());
    // A v1 stylesheet.
    const CompileResult v1 = compileFiles({SourceFile{
        "v.xt",
        "<xt><head><style>#A { offset: 1px 2px; }</style></head><body><div id=\"A\"></div></body></xt>"}});
    CHECK_FALSE(v1.ok());
    CHECK(findCode(v1.diagnostics, "layout.notGeometry") != nullptr);
    // A declaration list (a `style` attribute); compileDocument() drops such an attribute whole with
    // its "invalid inline style" warning, as for any other error in it (T-23).
    DiagnosticSink sink;
    std::vector<StyleDecl> decls;
    CHECK_FALSE(parseDeclarations("width: 4px; anchor: left top", "s.xt", 1, sink, decls));
    CHECK(findCode(sink.all(), "layout.notGeometry") != nullptr);
}

#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xts_props.h"  // styleprop::* (internal; Code/Runtime/compile/src/xts is a test-only include dir).

/// Tests for the `.xts` rule/at-rule parser (Code/Runtime/compile/src/xts/xts_parser.cpp). See
/// Docs/design/05-style-and-css.md §5.3-5.6, Docs/tasks/T-22.md.

using namespace xt;
using namespace xt::compile;

namespace {

const StyleDecl* findDecl(const std::vector<StyleDecl>& decls, uint16_t prop) {
    for (const StyleDecl& d : decls) {
        if (d.prop == prop) {
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("parseXts: single rule, compound selector with combinator and pseudo-class") {
    DiagnosticSink sink;
    RuleSet rs;
    bool ok = parseXts("div.a#b:hover > .c { color: red }", "t.xts", 1, sink, rs);
    CHECK(ok);
    CHECK(sink.hasErrors() == false);
    REQUIRE(rs.rules.size() == 1);
    const StyleRule& rule = rs.rules[0];
    REQUIRE(rule.selectors.size() == 1);
    const Selector& sel = rule.selectors[0];
    REQUIRE(sel.parts.size() == 2);
    // parts[0] is the subject.
    CHECK(sel.parts[0].tag.empty());
    REQUIRE(sel.parts[0].classes.size() == 1);
    CHECK(sel.parts[0].classes[0] == "c");
    CHECK(sel.parts[0].id.empty());
    CHECK(sel.parts[0].stateMask == 0);
    // parts[1] is the ancestor div.a#b:hover, connected to the subject by a child combinator.
    CHECK(sel.parts[1].tag == "div");
    REQUIRE(sel.parts[1].classes.size() == 1);
    CHECK(sel.parts[1].classes[0] == "a");
    CHECK(sel.parts[1].id == "b");
    CHECK(sel.parts[1].stateMask == xt::kStateHover);
    CHECK(sel.parts[1].combinator == 1);  // child
    CHECK(sel.specificity == ((1u << 16) | (3u << 8) | 1u));

    REQUIRE(rule.decls.size() == 1);
    CHECK(rule.decls[0].prop == prop::Color);
    CHECK(rule.decls[0].value.kind == StyleValueKind::Color);
    CHECK(rule.decls[0].value.color.r == doctest::Approx(1.0f));
    CHECK(rule.decls[0].value.color.g == doctest::Approx(0.0f));
    CHECK(rule.decls[0].value.color.b == doctest::Approx(0.0f));
}

TEST_CASE("parseXts: grouped selector list keeps both selectors on one rule") {
    DiagnosticSink sink;
    RuleSet rs;
    bool ok = parseXts("a, b { }", "t.xts", 1, sink, rs);
    CHECK(ok);
    REQUIRE(rs.rules.size() == 1);
    REQUIRE(rs.rules[0].selectors.size() == 2);
    CHECK(rs.rules[0].selectors[0].parts[0].tag == "a");
    CHECK(rs.rules[0].selectors[1].parts[0].tag == "b");
    CHECK(rs.rules[0].decls.empty());
}

TEST_CASE("parseXts: multiple rules, comments, quoted string with ';', !important") {
    DiagnosticSink sink;
    RuleSet rs;
    std::string src =
        "/* leading comment */\n"
        ".x { color: red; /* inline */ }\n"
        ".y { font-family: \"a;b\"; color: blue !important; }\n";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok);
    REQUIRE(rs.rules.size() == 2);
    REQUIRE(rs.rules[1].decls.size() == 2);
    const StyleDecl* fam = findDecl(rs.rules[1].decls, styleprop::FontFamily);
    REQUIRE(fam != nullptr);
    REQUIRE(fam->value.families.size() == 1);
    CHECK(fam->value.families[0] == "a;b");
    const StyleDecl* color = findDecl(rs.rules[1].decls, prop::Color);
    REQUIRE(color != nullptr);
    CHECK(color->important);
}

TEST_CASE("parseXts: error recovery -- invalid value warns and drops the decl, rule still parses") {
    DiagnosticSink sink;
    RuleSet rs;
    std::string src = "a { color: blue; }\nx { color: ; }\nb { color: green; }\n";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok);  // Only a warning, no error.
    CHECK(sink.hasErrors() == false);
    CHECK(sink.warningCount() >= 1);
    REQUIRE(rs.rules.size() == 3);
    CHECK(rs.rules[1].decls.empty());
    CHECK(findDecl(rs.rules[0].decls, prop::Color) != nullptr);
    CHECK(findDecl(rs.rules[2].decls, prop::Color) != nullptr);
}

TEST_CASE("parseXts: error recovery -- missing ':' errors the rule and skips to the next '}'") {
    DiagnosticSink sink;
    RuleSet rs;
    // "y"'s declaration is missing ':'; recovery skips forward to the next '}' (y's own), then the
    // third rule "b" parses normally.
    std::string src = "a { color: blue; }\ny { color red; }\nb { color: green; }\n";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    REQUIRE(rs.rules.size() == 2);  // "y" is dropped entirely.
    CHECK(rs.rules[0].selectors[0].parts[0].tag == "a");
    CHECK(rs.rules[1].selectors[0].parts[0].tag == "b");
    CHECK(findDecl(rs.rules[1].decls, prop::Color) != nullptr);
}

TEST_CASE("parseXts: error recovery -- unterminated block at EOF") {
    DiagnosticSink sink;
    RuleSet rs;
    std::string src = "a { color: blue; }\ny { color: red";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    REQUIRE(rs.rules.size() == 1);
    CHECK(rs.rules[0].selectors[0].parts[0].tag == "a");
}

TEST_CASE("parseXts: @keyframes from/50%/to, timing extraction, multi-percent expansion") {
    DiagnosticSink sink;
    RuleSet rs;
    std::string src =
        "@keyframes spin {\n"
        "  from { opacity: 0; }\n"
        "  50% { opacity: 0.5; animation-timing-function: ease-in; }\n"
        "  to { opacity: 1; }\n"
        "}\n"
        "@keyframes blink { 0%, 100% { opacity: 1; } }\n";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok);
    REQUIRE(rs.keyframes.size() == 2);
    const KeyframesDef& spin = rs.keyframes[0];
    CHECK(spin.name == "spin");
    REQUIRE(spin.stops.size() == 3);
    CHECK(spin.stops[0].offset == doctest::Approx(0));
    CHECK(spin.stops[0].hasTiming == false);
    CHECK(spin.stops[1].offset == doctest::Approx(50));
    CHECK(spin.stops[1].hasTiming);
    CHECK(spin.stops[1].timing.kind == EasingKind::CubicBezier);
    // The timing decl itself must not leak into decls.
    CHECK(spin.stops[1].decls.size() == 1);
    CHECK(spin.stops[2].offset == doctest::Approx(100));

    const KeyframesDef& blink = rs.keyframes[1];
    REQUIRE(blink.stops.size() == 2);
    CHECK(blink.stops[0].offset == doctest::Approx(0));
    CHECK(blink.stops[1].offset == doctest::Approx(100));
}

TEST_CASE("parseXts: @font-face complete, and missing src is an error") {
    DiagnosticSink sink;
    RuleSet rs;
    std::string ok1 =
        "@font-face { font-family: \"MyFont\"; src: \"fonts/x.ttf\"; font-weight: 700; font-style: "
        "italic; }";
    bool r1 = parseXts(ok1, "t.xts", 1, sink, rs);
    CHECK(r1);
    REQUIRE(rs.fontFaces.size() == 1);
    CHECK(rs.fontFaces[0].family == "MyFont");
    CHECK(rs.fontFaces[0].src == "fonts/x.ttf");
    CHECK(rs.fontFaces[0].weight == 700);
    CHECK(rs.fontFaces[0].italic);

    DiagnosticSink sink2;
    RuleSet rs2;
    std::string missingSrc = "@font-face { font-family: \"X\"; }";
    bool r2 = parseXts(missingSrc, "t.xts", 1, sink2, rs2);
    CHECK(r2 == false);
    CHECK(sink2.hasErrors());
    CHECK(rs2.fontFaces.empty());
}

TEST_CASE("parseXts: unknown at-rule is skipped, including nested braces") {
    DiagnosticSink sink;
    RuleSet rs;
    // T-176: @media is a known at-rule now; @supports stands in for "unknown".
    std::string src = "@supports (display: grid) { a { color: red; } }\nb { color: blue; }";
    bool ok = parseXts(src, "t.xts", 1, sink, rs);
    CHECK(ok);
    CHECK(sink.warningCount() >= 1);
    REQUIRE(rs.rules.size() == 1);
    CHECK(rs.rules[0].selectors[0].parts[0].tag == "b");
}

TEST_CASE("parseXts: unknown pseudo-class errors, unknown property warns") {
    DiagnosticSink sink;
    RuleSet rs;
    bool ok = parseXts(".a:visited { color: red; }", "t.xts", 1, sink, rs);
    CHECK(ok == false);
    CHECK(sink.hasErrors());
    CHECK(rs.rules.empty());

    DiagnosticSink sink2;
    RuleSet rs2;
    bool ok2 = parseXts(".b { frobnicate: 1; color: red; }", "t.xts", 1, sink2, rs2);
    CHECK(ok2);
    CHECK(sink2.warningCount() >= 1);
    REQUIRE(rs2.rules.size() == 1);
    CHECK(rs2.rules[0].decls.size() == 1);
}

TEST_CASE("parseXts: lineOffset shifts diagnostic line numbers") {
    DiagnosticSink sink;
    RuleSet rs;
    // Simulate a <style> block starting at source line 10; the bad declaration is on the block's
    // 2nd line, so it should be reported as file line 11.
    std::string src = ".a { color: red; }\n.b { color: ; }\n";
    bool ok = parseXts(src, "page.xt", 10, sink, rs);
    CHECK(ok);
    REQUIRE(sink.all().size() >= 1);
    bool found = false;
    for (const Diagnostic& d : sink.all()) {
        if (d.line == 11) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("parseDeclarations: bare declaration list") {
    DiagnosticSink sink;
    std::vector<StyleDecl> decls;
    bool ok = parseDeclarations("width:10px; color:#000", "style", 5, sink, decls);
    CHECK(ok);
    REQUIRE(decls.size() == 2);
    const StyleDecl* width = findDecl(decls, prop::Width);
    REQUIRE(width != nullptr);
    CHECK(width->value.len.value == doctest::Approx(10));
    CHECK(width->value.len.unit == LengthUnit::Px);
    const StyleDecl* color = findDecl(decls, prop::Color);
    REQUIRE(color != nullptr);
    CHECK(color->value.color.r == doctest::Approx(0));
}

// ---------------------------------------------------------------------------------------------
// T-322: `transition-*` longhands merge with the `transition` shorthand per declaration block,
// in source order (the later declaration wins).
// ---------------------------------------------------------------------------------------------

namespace {

const StyleDecl& onlyTransition(const RuleSet& rs) {
    REQUIRE(rs.rules.size() == 1);
    const StyleDecl* d = findDecl(rs.rules[0].decls, styleprop::Transition);
    REQUIRE(d != nullptr);
    size_t count = 0;
    for (const StyleDecl& decl : rs.rules[0].decls) {
        count += decl.prop == styleprop::Transition ? 1u : 0u;
    }
    CHECK(count == 1);
    return *d;
}

}  // namespace

TEST_CASE("T-322 parseXts: transition longhands alone, zipped like CSS lists") {
    DiagnosticSink sink;
    RuleSet rs;
    CHECK(
        parseXts("a { width: 1px; transition-property: opacity, width; transition-duration: 200ms;"
                 " transition-timing-function: step-end, ease-in; transition-delay: 1s; height: 2px }",
                 "t.xts", 1, sink, rs));
    CHECK(sink.all().empty());
    const StyleDecl& d = onlyTransition(rs);
    REQUIRE(d.value.transitions.size() == 2);
    CHECK(d.value.transitions[0].prop == prop::Opacity);
    CHECK(d.value.transitions[0].duration == doctest::Approx(0.2f));  // One duration repeats.
    CHECK(d.value.transitions[0].easing.kind == EasingKind::Steps);
    CHECK(d.value.transitions[0].delay == doctest::Approx(1));
    CHECK(d.value.transitions[1].prop == prop::Width);
    CHECK(d.value.transitions[1].duration == doctest::Approx(0.2f));
    CHECK(d.value.transitions[1].easing.kind == EasingKind::CubicBezier);
    // The merged declaration sits where the last transition declaration was.
    const std::vector<StyleDecl>& decls = rs.rules[0].decls;
    REQUIRE(decls.size() == 3);
    CHECK(decls[0].prop == prop::Width);
    CHECK(decls[1].prop == styleprop::Transition);
    CHECK(decls[2].prop == prop::Height);

    // Only a duration: the property defaults to `all`.
    DiagnosticSink sink2;
    RuleSet rs2;
    CHECK(parseXts("a { transition-duration: 1s }", "t.xts", 1, sink2, rs2));
    const StyleDecl& d2 = onlyTransition(rs2);
    REQUIRE(d2.value.transitions.size() == 1);
    CHECK(d2.value.transitions[0].prop == 0xFFFF);
    CHECK(d2.value.transitions[0].duration == doctest::Approx(1));

    // `transition-property: none` is no transition at all.
    DiagnosticSink sink3;
    RuleSet rs3;
    CHECK(parseXts("a { transition: opacity 1s; transition-property: none }", "t.xts", 1, sink3, rs3));
    CHECK(sink3.all().empty());
    CHECK(onlyTransition(rs3).value.transitions.empty());
}

TEST_CASE("T-322 parseXts: a longhand after the shorthand overrides that component") {
    DiagnosticSink sink;
    RuleSet rs;
    CHECK(parseXts("a { transition: opacity 1s ease-in 2s, width 3s; transition-duration: 500ms }", "t.xts",
                   1, sink, rs));
    CHECK(sink.all().empty());
    const StyleDecl& d = onlyTransition(rs);
    REQUIRE(d.value.transitions.size() == 2);
    CHECK(d.value.transitions[0].prop == prop::Opacity);
    CHECK(d.value.transitions[0].duration == doctest::Approx(0.5f));
    CHECK(d.value.transitions[0].easing.kind == EasingKind::CubicBezier);
    CHECK(d.value.transitions[0].delay == doctest::Approx(2));
    CHECK(d.value.transitions[1].prop == prop::Width);
    CHECK(d.value.transitions[1].duration == doctest::Approx(0.5f));
    CHECK(d.value.transitions[1].delay == doctest::Approx(0));
}

TEST_CASE("T-322 parseXts: the shorthand after a longhand resets it") {
    DiagnosticSink sink;
    RuleSet rs;
    CHECK(parseXts("a { transition-duration: 5s; transition-delay: 1s; transition: opacity 1s }", "t.xts", 1,
                   sink, rs));
    CHECK(sink.all().empty());
    const StyleDecl& d = onlyTransition(rs);
    REQUIRE(d.value.transitions.size() == 1);
    CHECK(d.value.transitions[0].prop == prop::Opacity);
    CHECK(d.value.transitions[0].duration == doctest::Approx(1));
    CHECK(d.value.transitions[0].delay == doctest::Approx(0));

    // A shorthand alone stays exactly what it was (no merge).
    DiagnosticSink sink2;
    RuleSet rs2;
    CHECK(parseXts("a { transition: opacity 1s; color: red }", "t.xts", 1, sink2, rs2));
    REQUIRE(rs2.rules[0].decls.size() == 2);
    CHECK(rs2.rules[0].decls[0].prop == styleprop::Transition);

    // !important follows the last transition declaration of the block.
    DiagnosticSink sink3;
    RuleSet rs3;
    CHECK(parseXts("a { transition: opacity 1s; transition-delay: 1s !important }", "t.xts", 1, sink3, rs3));
    CHECK(onlyTransition(rs3).important);
}

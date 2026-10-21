#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "mini_css.h"      // core/src (on the test include path).
#include "style_decode.h"  // core/src
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/xtfx.h"
#include "xt/document.h"
#include "xt/particles.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/value_helpers.h"
#include "xts_lexer.h"  // compile/src/xts (on the test include path).
#include "xts_props.h"  // compile::styleprop::Animation.

/// T-272: CSS Syntax Level 3 numbers (exponent included) in every reader of style / attribute
/// numbers, and an `animation` iteration count of 0, which does not play (CSS) instead of playing
/// forever -- in the compiler and in the runtime mini CSS.

using namespace xt;

namespace {

const compile::StyleDecl* findDecl(const std::vector<compile::StyleDecl>& decls, uint16_t prop) {
    for (const compile::StyleDecl& d : decls) {
        if (d.prop == prop) {
            return &d;
        }
    }
    return nullptr;
}

std::vector<compile::StyleDecl> parseOneRule(const std::string& body, compile::DiagnosticSink& sink) {
    compile::RuleSet rs;
    compile::parseXts("a { " + body + " }", "t.xts", 1, sink, rs);
    if (rs.rules.empty()) {
        return {};
    }
    return rs.rules[0].decls;
}

std::string page(std::string_view body, std::string_view style = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

compile::CompileResult compileSrc(const std::string& xt) {
    compile::CompileOptions options;
    options.emitMeta = false;
    std::vector<compile::SourceFile> files{{"main.xt", xt}};
    return compile::compileDocumentFromStrings(files, "main.xt", options);
}

bool hasDiagnostic(const compile::CompileResult& r, compile::Diagnostic::Level level,
                   std::string_view needle) {
    for (const compile::Diagnostic& d : r.diagnostics) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<Document> load(const compile::CompileResult& r) {
    for (const compile::Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 200, 200});
    return doc;
}

}  // namespace

// -- Exponent notation ----------------------------------------------------------------------------

TEST_CASE("T-272 .xts lexer: a number takes an exponent only when a digit follows the e") {
    struct Case {
        const char* text;
        float num;
        const char* unit;
    };
    for (const Case& c : {Case{"1e2px", 100.0f, "px"}, Case{"1E-1", 0.1f, ""}, Case{"1.5e+2", 150.0f, ""},
                          Case{"-2.5E3%", -2500.0f, "%"}, Case{".5e1s", 5.0f, "s"}, Case{"1em", 1.0f, "em"},
                          Case{"2ex", 2.0f, "ex"}, Case{"1e", 1.0f, "e"}, Case{"1e-x", 1.0f, "e-x"},
                          Case{"3E+px", 3.0f, "E"}}) {
        CAPTURE(c.text);
        compile::Lexer lexer(c.text);
        const compile::Token t = lexer.next();
        CHECK(t.kind == compile::TokenKind::Number);
        CHECK(t.num == doctest::Approx(c.num));
        CHECK(t.unit == c.unit);
        if (std::string_view(c.text) == "3E+px") {
            CHECK(lexer.next().kind == compile::TokenKind::Unknown);  // The '+', as before.
            CHECK(lexer.next().text == "px");
        }
        CHECK(lexer.next().kind == compile::TokenKind::End);
    }
    // A number beyond float is not a number token (the declaration then is an invalid value).
    for (const char* text : {"1e999", "1e999px", "-1e39"}) {
        CAPTURE(text);
        compile::Lexer lexer(text);
        CHECK(lexer.next().kind == compile::TokenKind::Unknown);
        CHECK(lexer.next().kind == compile::TokenKind::End);
    }
    // A magnitude below float (and below double) reads as zero, with or without an exponent.
    const std::string tiny = "0." + std::string(400, '0') + "1px";
    for (const char* text : {"1e-50px", "1e-999px", "-12.5E-400px", "0.001e-320px", tiny.c_str()}) {
        CAPTURE(text);
        compile::Lexer lexer(text);
        const compile::Token t = lexer.next();
        CHECK(t.kind == compile::TokenKind::Number);
        CHECK(t.num == 0.0f);
        CHECK(t.unit == "px");
    }
    // Digits alone can overflow too.
    const std::string huge = "1" + std::string(400, '0') + ".5";
    compile::Lexer lexer(huge);
    CHECK(lexer.next().kind == compile::TokenKind::Unknown);
}

TEST_CASE("T-272 .xts values: exponent numbers in declarations; non-finite is an invalid value") {
    compile::DiagnosticSink sink;
    auto decls = parseOneRule(
        "width: 1e2px; opacity: 1E-1; height: 1.5e+2px; margin-top: 1em; "
        "line-height: 2e0;",
        sink);
    CHECK(sink.all().empty());
    REQUIRE(findDecl(decls, prop::Width) != nullptr);
    CHECK(findDecl(decls, prop::Width)->value.len.value == doctest::Approx(100));
    CHECK(findDecl(decls, prop::Width)->value.len.unit == LengthUnit::Px);
    REQUIRE(findDecl(decls, prop::Opacity) != nullptr);
    CHECK(findDecl(decls, prop::Opacity)->value.f == doctest::Approx(0.1));
    REQUIRE(findDecl(decls, prop::Height) != nullptr);
    CHECK(findDecl(decls, prop::Height)->value.len.value == doctest::Approx(150));
    REQUIRE(findDecl(decls, prop::MarginTop) != nullptr);
    CHECK(findDecl(decls, prop::MarginTop)->value.len.unit == LengthUnit::Em);
    CHECK(findDecl(decls, prop::MarginTop)->value.len.value == doctest::Approx(1));
    REQUIRE(findDecl(decls, prop::LineHeight) != nullptr);
    CHECK(findDecl(decls, prop::LineHeight)->value.len.value == doctest::Approx(2));

    // `1e999px` takes the same path as any other value that is not a length.
    for (const char* value : {"1e999px", "bogus"}) {
        CAPTURE(value);
        compile::DiagnosticSink bad;
        auto none = parseOneRule(std::string("width: ") + value + ";", bad);
        CHECK(findDecl(none, prop::Width) == nullptr);
        REQUIRE(bad.all().size() == 1);
        CHECK(bad.all()[0].level == compile::Diagnostic::Level::Warning);
        CHECK(bad.all()[0].message == "invalid value for \"width\"");
    }
}

TEST_CASE("T-272 .xts particle properties and attributes take exponent numbers") {
    const compile::CompileResult r =
        compileSrc(page(R"~(<particles id="P" rate="2e1" max="8" lifetime="5"></particles>
        <particles id="Q" max="8" lifetime="5"></particles>)~",
                        "particles { width: 10px; height: 10px; } #Q { particle-rate: 1e3; }"));
    std::unique_ptr<Document> doc = load(r);  // The Document borrows r's IR bytes.
    doc->update(0);
    const ParticleNodeData* p = doc->particleData(doc->byId("P").node);
    REQUIRE(p != nullptr);
    CHECK(p->rate == doctest::Approx(20));
    const ParticleNodeData* q = doc->particleData(doc->byId("Q").node);
    REQUIRE(q != nullptr);
    CHECK(q->rate == doctest::Approx(1000));
}

TEST_CASE("T-272 runtime mini CSS: exponent numbers; non-finite is invalid") {
    std::vector<mini_css::Decl> decls;
    CHECK(mini_css::parseDeclarations("width: 1e2px; opacity: 1E-1; height: 1.5e+2px; margin-top: 1em",
                                      decls) == Status::kOk);
    REQUIRE(decls.size() == 4);
    CHECK(decls[0].prop == prop::Width);
    CHECK(decls[0].value.len.value == doctest::Approx(100));
    CHECK(decls[0].value.len.unit == LengthUnit::Px);
    CHECK(decls[1].value.f == doctest::Approx(0.1));
    CHECK(decls[2].value.len.value == doctest::Approx(150));
    CHECK(decls[3].value.len.unit == LengthUnit::Em);
    CHECK(decls[3].value.len.value == doctest::Approx(1));

    decls.clear();
    CHECK(mini_css::parseDeclarations("width: 1e999px", decls) == Status::kInvalidArgument);
    CHECK(decls.empty());
    CHECK(parseValue(prop::Width, "1e2px").ok());
}

TEST_CASE("T-272 bindings, triggers, <camera> and .xtfx numbers: exponent yes, non-finite no") {
    // Bindings.
    compile::CompileResult bind =
        compileSrc(page(R"X(<div id="A" port:hp="number" bind:width="hp | scale(1e1)"></div>)X"));
    CHECK(bind.ok());
    bind = compileSrc(page(R"X(<div id="A" port:hp="number" bind:width="hp | scale(1e999)"></div>)X"));
    CHECK(!bind.ok());
    CHECK(hasDiagnostic(bind, compile::Diagnostic::Level::Error, "neither a number"));

    // Triggers.
    compile::CompileResult trig =
        compileSrc(page(R"X(<div id="A" port:hp="number" on:port(hp)="if lt(1e1) hide"></div>)X"));
    CHECK(trig.ok());
    trig = compileSrc(page(R"X(<div id="A" port:hp="number" on:port(hp)="if lt(1e999) hide"></div>)X"));
    CHECK(!trig.ok());
    CHECK(hasDiagnostic(trig, compile::Diagnostic::Level::Error, "compares against numbers"));

    // <camera> attributes.
    compile::CompileResult cam = compileSrc(page(R"X(<div><camera id="cam" zoom="4e2"/></div>)X"));
    CHECK(cam.ok());
    CHECK(!hasDiagnostic(cam, compile::Diagnostic::Level::Warning, "zoom"));
    cam = compileSrc(page(R"X(<div><camera id="cam" zoom="1e999"/></div>)X"));
    CHECK(hasDiagnostic(cam, compile::Diagnostic::Level::Warning, "invalid \"zoom\" for <camera>"));
    // Leading whitespace stays accepted, trailing text stays rejected (as before T-272).
    cam = compileSrc(page(R"X(<div><camera id="cam" zoom=" 2"/></div>)X"));
    CHECK(!hasDiagnostic(cam, compile::Diagnostic::Level::Warning, "zoom"));
    cam = compileSrc(page(R"X(<div><camera id="cam" zoom="2 "/></div>)X"));
    CHECK(hasDiagnostic(cam, compile::Diagnostic::Level::Warning, "invalid \"zoom\" for <camera>"));

    // .xtfx parameters: CSS numbers only (no hexadecimal float, no inf).
    compile::XtfxHeader header;
    std::string body;
    int bodyLine = 0;
    compile::XtfxError err;
    REQUIRE(compile::parseXtfxHeader("param k float 1e-1 [0, 1e1]\n---\n", header, body, bodyLine, err));
    REQUIRE(header.params.size() == 1);
    CHECK(header.params[0].def[0] == doctest::Approx(0.1));
    CHECK(header.params[0].max == doctest::Approx(10));
    for (const char* bad :
         {"param k float 0x10\n---\n", "param k float inf\n---\n", "param k float 1e999\n---\n"}) {
        CAPTURE(bad);
        compile::XtfxHeader h;
        CHECK(!compile::parseXtfxHeader(bad, h, body, bodyLine, err));
    }
}

// -- animation iteration count 0 ------------------------------------------------------------------

TEST_CASE("T-272 .xts animation: an iteration count of 0 removes the item with a warning") {
    {
        compile::DiagnosticSink sink;
        auto decls = parseOneRule("animation: spin 1s 0;", sink);
        const compile::StyleDecl* d = findDecl(decls, compile::styleprop::Animation);
        REQUIRE(d != nullptr);  // An empty list: `animation: none`.
        CHECK(d->value.animations.empty());
        REQUIRE(sink.all().size() == 1);
        CHECK(sink.all()[0].level == compile::Diagnostic::Level::Warning);
        CHECK(sink.all()[0].message == "animation \"spin\": iteration count 0, the animation is not played");
    }
    {
        compile::DiagnosticSink sink;
        auto decls = parseOneRule("animation: a 1s 0, b 1s 2;", sink);
        const compile::StyleDecl* d = findDecl(decls, compile::styleprop::Animation);
        REQUIRE(d != nullptr);
        REQUIRE(d->value.animations.size() == 1);
        CHECK(d->value.animations[0].name == "b");
        CHECK(d->value.animations[0].iterations == 2);
        REQUIRE(sink.all().size() == 1);
        CHECK(sink.all()[0].message == "animation \"a\": iteration count 0, the animation is not played");
    }
    {
        // A fractional count still truncates, so 0.5 is 0 as well.
        compile::DiagnosticSink sink;
        auto decls = parseOneRule("animation: spin 1s 0.5;", sink);
        REQUIRE(findDecl(decls, compile::styleprop::Animation) != nullptr);
        CHECK(findDecl(decls, compile::styleprop::Animation)->value.animations.empty());
        CHECK(sink.warningCount() == 1);
    }
    {
        compile::DiagnosticSink sink;
        auto decls = parseOneRule("animation: spin 1s infinite;", sink);
        const compile::StyleDecl* d = findDecl(decls, compile::styleprop::Animation);
        REQUIRE(d != nullptr);
        REQUIRE(d->value.animations.size() == 1);
        CHECK(d->value.animations[0].iterations == 0xFFFF);
        CHECK(sink.all().empty());
    }
}

TEST_CASE("T-272 compiled animation: count 0 does not play, infinite still does") {
    const std::string style =
        "@keyframes spin { from { opacity: 0 } to { opacity: 1 } } "
        "div { width: 10px; height: 10px; } "
        "#Zero { animation: spin 1s 0; } #Mixed { animation: spin 1s 0, spin 2s 2; } "
        "#Inf { animation: spin 1s infinite; }";
    compile::CompileResult r =
        compileSrc(page(R"(<div id="Zero"></div><div id="Mixed"></div><div id="Inf"></div>)", style));
    CHECK(hasDiagnostic(r, compile::Diagnostic::Level::Warning,
                        "animation \"spin\": iteration count 0, the animation is not played"));
    std::unique_ptr<Document> doc = load(r);
    doc->update(0.016f);
    CHECK(doc->animationsOf(doc->byId("Zero")).empty());
    CHECK(doc->animationsOf(doc->byId("Mixed")).size() == 1);
    CHECK(doc->animationsOf(doc->byId("Inf")).size() == 1);
    doc->update(5.0f);
    CHECK(doc->animationsOf(doc->byId("Inf")).size() == 1);  // Still playing: infinite.
}

TEST_CASE("T-272 runtime mini CSS animation: an iteration count of 0 creates no animation") {
    std::vector<mini_css::Decl> decls;
    CHECK(mini_css::parseDeclarations("animation: spin 1s 0", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].prop == style_decode::styleprop::Animation);
    CHECK(decls[0].animations.empty());

    decls.clear();
    CHECK(mini_css::parseDeclarations("animation: a 1s 0.5, b 1s 2", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].name == "b");
    CHECK(decls[0].animations[0].spec.iterations == 2);

    decls.clear();
    CHECK(mini_css::parseDeclarations("animation: spin 1s infinite", decls) == Status::kOk);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].spec.iterations == 0);  // IrAnimationSpec: 0 = infinite.

    // Through ElementRef::setStyle: nothing plays.
    const compile::CompileResult r =
        compileSrc(page(R"(<div id="Box"></div>)",
                        "@keyframes spin { from { opacity: 0 } to { opacity: 1 } } "
                        "div { width: 10px; height: 10px; }"));
    std::unique_ptr<Document> doc = load(r);
    ElementRef box = doc->byId("Box");
    REQUIRE(box.valid());
    CHECK(box.setStyle("animation: spin 1s 0") == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->animationsOf(box).empty());
    CHECK(box.setStyle("animation: spin 1s 0, spin 2s 2") == Status::kOk);
    doc->update(0.032f);
    CHECK(doc->animationsOf(box).size() == 1);
}

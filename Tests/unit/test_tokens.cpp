#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-170: native `var()` design tokens and themes. See Docs/design/05-style-and-css.md
/// §5.9.2-5.9.3, 12-compiler.md §12.7, 13-api.md §13.8.2, Docs/tasks/T-170.md "測試案例".

using namespace xt;

namespace {

struct Compiled {
    std::vector<uint8_t> ir;
    std::vector<compile::Diagnostic> diagnostics;
};

/// Compiles `<xt><head><style>css</style></head><body>body</body></xt>` in memory.
Compiled compileDoc(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"tokens.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "tokens.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return Compiled{result.ir, result.diagnostics};
}

size_t countWarnings(const Compiled& c, const std::string& needle) {
    return static_cast<size_t>(std::count_if(c.diagnostics.begin(), c.diagnostics.end(), [&](const auto& d) {
        return d.message.find(needle) != std::string::npos;
    }));
}

/// One Context + one loaded Document, updated once.
struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;

    explicit Loaded(const std::vector<uint8_t>& ir) {
        ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.update(0.0f);
    }

    const ComputedStyle& style(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return doc->arena().element(ref.node).style;
    }
    Color color(const char* id, PropId p) { return style(id).base[p].c; }
    float length(const char* id, PropId p) { return style(id).base[p].len.value; }
};

const Color kRed{1, 0, 0, 1};
const Color kBlue{0, 0, 1, 1};
const Color kGreen{0, 1, 0, 1};

}  // namespace

TEST_CASE("T-170: a token is compiled into VARS and the declaration keeps a reference") {
    Compiled c = compileDoc(":root{--c:#f00} .a{color:var(--c)}", R"(<div id="a" class="a"></div>)");
    CHECK(c.diagnostics.empty());
    Result<IrView> view = IrView::open(IrBlob{c.ir.data(), c.ir.size()});
    REQUIRE(view.ok());
    REQUIRE(view.value().vars().present());
    REQUIRE(view.value().vars().tokens().size() == 1);
    const IrVar& var = view.value().vars().tokens()[0];
    CHECK(view.value().string(var.name) == "--c");
    CHECK(var.valueType == static_cast<uint8_t>(ValueType::Color));
    bool sawRef = false;
    for (const IrDecl& d : view.value().style().decls()) {
        if (d.valueType == kDeclVarRef) {
            sawRef = true;
            CHECK(view.value().string(irValueU32(d.value, 0)) == "--c");
        }
    }
    CHECK(sawRef);

    // The round trip through IrWriter::fromView keeps VARS byte-for-byte.
    std::vector<uint8_t> again = compile::IrWriter::fromView(view.value()).finish();
    CHECK(again == c.ir);
}

TEST_CASE("T-170: .a is red, setToken turns it blue on the next frame without a reload") {
    Compiled c = compileDoc(":root{--c:#f00} .a{color:var(--c)}", R"(<div id="a" class="a"></div>)");
    Loaded l(c.ir);
    CHECK(l.color("a", prop::Color) == kRed);

    const uint8_t* blobBefore = l.doc->ir().blob().data;
    Document* docBefore = l.doc;
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kBlue)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kBlue);
    // Nothing was recompiled or reloaded: same Document, same IR bytes.
    CHECK(l.ctx.document(1) == docBefore);
    CHECK(l.doc->ir().blob().data == blobBefore);

    CHECK(l.ctx.setToken("c", Value::ofColor(kBlue)) == Status::kInvalidArgument);
    CHECK(l.ctx.setToken("--c", Value::ofStringId(0)) == Status::kInvalidArgument);
}

TEST_CASE("T-187: clearToken drops one override, clearTokens drops them all") {
    Compiled c = compileDoc(":root{--c:#f00; --w:10px} .a{color:var(--c); width:var(--w); height:2px}",
                            R"(<div id="a" class="a"></div>)");
    Loaded l(c.ir);
    const std::vector<TokenDef> alt{{"--c", "rgb(0, 255, 0)"}};
    REQUIRE(l.ctx.defineTheme("alt", alt) == Status::kOk);
    REQUIRE(l.ctx.setTheme("alt") == Status::kOk);
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kBlue)) == Status::kOk);
    REQUIRE(l.ctx.setToken("--w", Value::ofLength(30, LengthUnit::Px)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kBlue);
    CHECK(l.length("a", prop::Width) == doctest::Approx(30));

    // Clearing --c falls back to the active theme; --w keeps its override.
    REQUIRE(l.ctx.clearToken("--c") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kGreen);
    CHECK(l.length("a", prop::Width) == doctest::Approx(30));
    CHECK(l.ctx.clearToken("--c") == Status::kNotFound);
    CHECK(l.ctx.clearToken("c") == Status::kInvalidArgument);

    // clearTokens() drops the rest; the base VARS value comes back.
    l.ctx.clearTokens();
    l.ctx.update(0.0f);
    CHECK(l.length("a", prop::Width) == doctest::Approx(10));
    CHECK(l.ctx.clearToken("--w") == Status::kNotFound);
    l.ctx.clearTokens();  // Nothing left: a no-op.
    REQUIRE(l.ctx.setTheme("") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kRed);
}

TEST_CASE("T-170: var() fallback, and an undefined token warns and uses the initial value") {
    Compiled withFallback =
        compileDoc(".a{width:var(--missing, 8px); height:4px}", R"(<div id="a" class="a"></div>)");
    CHECK(withFallback.diagnostics.empty());
    Loaded a(withFallback.ir);
    CHECK(a.length("a", prop::Width) == doctest::Approx(8));

    // `div` sets 0.5 with lower specificity; `.b`'s unresolvable var() wins the cascade and so
    // yields opacity's initial value 1, not the 0.5 below it.
    Compiled missing =
        compileDoc("div{opacity:0.5} .b{opacity:var(--missing)}", R"(<div id="b" class="b"></div>)");
    CHECK(missing.diagnostics.size() == 1);
    CHECK(countWarnings(missing, "var(--missing) is undefined") == 1);
    Loaded b(missing.ir);
    CHECK(b.style("b").base[prop::Opacity].f == doctest::Approx(1));
}

TEST_CASE("T-170: themes lay over the base values and switch without recompiling") {
    Compiled c = compileDoc(":root{--c:#f00; --w:10px} .a{color:var(--c); width:var(--w); height:2px}",
                            R"(<div id="a" class="a"></div>)");
    Loaded l(c.ir);
    const std::vector<TokenDef> dark{{"--c", "#0000ff"}, {"--w", "30px"}};
    const std::vector<TokenDef> alt{{"--c", "rgb(0, 255, 0)"}};
    REQUIRE(l.ctx.defineTheme("dark", dark) == Status::kOk);
    REQUIRE(l.ctx.defineTheme("alt", alt) == Status::kOk);
    CHECK(l.ctx.setTheme("nope") == Status::kNotFound);
    CHECK(l.ctx.defineTheme("bad", std::vector<TokenDef>{{"c", "#fff"}}) == Status::kInvalidArgument);

    REQUIRE(l.ctx.setTheme("dark") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.ctx.theme() == "dark");
    CHECK(l.color("a", prop::Color) == kBlue);
    CHECK(l.length("a", prop::Width) == doctest::Approx(30));

    REQUIRE(l.ctx.setTheme("alt") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kGreen);
    CHECK(l.length("a", prop::Width) == doctest::Approx(10));  // Not in "alt": the base value.

    // setToken() sits above the active theme.
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kRed)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kRed);

    REQUIRE(l.ctx.setTheme("") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.length("a", prop::Width) == doctest::Approx(10));
}

TEST_CASE("T-170: an inherited token change reaches the children") {
    Compiled c = compileDoc(":root{--c:#f00} #p{color:var(--c)}", R"(<div id="p"><div id="k"></div></div>)");
    Loaded l(c.ir);
    CHECK(l.color("k", prop::Color) == kRed);
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kBlue)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("k", prop::Color) == kBlue);
}

TEST_CASE("T-170: a box shorthand feeds every longhand; a nested var() is substituted") {
    Compiled c = compileDoc(
        ":root{--sp:6px; --edge:#0000ff}"
        " .a{padding:var(--sp); border:2px var(--edge); width:10px; height:10px}",
        R"(<div id="a" class="a"></div>)");
    CHECK(c.diagnostics.empty());
    Loaded l(c.ir);
    CHECK(l.length("a", prop::PaddingLeft) == doctest::Approx(6));
    CHECK(l.length("a", prop::PaddingTop) == doctest::Approx(6));
    CHECK(l.color("a", prop::BorderColor) == kBlue);
    REQUIRE(l.ctx.setToken("--sp", Value::ofLength(3, LengthUnit::Px)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.length("a", prop::PaddingRight) == doctest::Approx(3));
}

TEST_CASE("T-170: token-to-token references, duplicates and misplaced declarations") {
    Compiled c = compileDoc(":root{--base:#00ff00; --c:var(--base)} .x{--c:var(--base)} .a{color:var(--c)}",
                            R"(<div id="a" class="a"></div>)");
    CHECK(countWarnings(c, "declared more than once") == 1);
    Loaded l(c.ir);
    CHECK(l.color("a", prop::Color) == kGreen);

    Compiled cycle = compileDoc(":root{--a:var(--b); --b:var(--a)} .a{color:var(--a, #f00)}",
                                R"(<div id="a" class="a"></div>)");
    CHECK(countWarnings(cycle, "refers to an undefined token or to itself") == 2);
    Loaded m(cycle.ir);
    CHECK(m.color("a", prop::Color) == kRed);
}

TEST_CASE("T-170: a token whose value does not fit the property warns and falls back") {
    Compiled c = compileDoc(":root{--c:12px} .a{color:var(--c, #00ff00)}", R"(<div id="a" class="a"></div>)");
    CHECK(countWarnings(c, "is not a valid \"color\" value") == 1);
    Loaded l(c.ir);
    CHECK(l.color("a", prop::Color) == kGreen);
}

TEST_CASE("T-170: var() inside @keyframes and a style attribute") {
    Compiled c =
        compileDoc(":root{--c:#0000ff} @keyframes k { from { opacity: var(--o, 0.25) } to { opacity: 1 } }",
                   R"x(<div id="a" style="color: var(--c)"></div>)x");
    CHECK(c.diagnostics.empty());
    Loaded l(c.ir);
    CHECK(l.color("a", prop::Color) == kBlue);
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kRed)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::Color) == kRed);
}

TEST_CASE("T-170: ElementRef::setStyle keeps a var() reference") {
    Compiled c = compileDoc(":root{--c:#f00}", R"(<div id="a"></div>)");
    Loaded l(c.ir);
    ElementRef a = l.doc->byId("a");
    REQUIRE(a.valid());
    CHECK(a.setStyle("background-color: var(--c); border-color: var(--none, #00ff00)") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::BackgroundColor) == kRed);
    CHECK(l.color("a", prop::BorderColor) == kGreen);
    REQUIRE(l.ctx.setToken("--c", Value::ofColor(kBlue)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.color("a", prop::BackgroundColor) == kBlue);
    // Nested var() is outside the runtime subset.
    CHECK(a.setStyle("transform: translateX(var(--c))") == Status::kInvalidArgument);
}

TEST_CASE("T-170: a document without tokens writes no VARS chunk and loads as before") {
    Compiled c = compileDoc(".a{color:#f00}", R"(<div id="a" class="a"></div>)");
    Result<IrView> view = IrView::open(IrBlob{c.ir.data(), c.ir.size()});
    REQUIRE(view.ok());
    CHECK(!view.value().vars().present());
    Loaded l(c.ir);
    CHECK(l.color("a", prop::Color) == kRed);
}

TEST_CASE("T-170: :root is accepted only on its own and only for tokens") {
    // A `style` attribute cannot declare a token (no element-level cascade).
    compile::DiagnosticSink inlineSink;
    std::vector<compile::StyleDecl> inlineDecls;
    CHECK(compile::parseDeclarations("--z: 1px; width: 2px", "t.xt", 1, inlineSink, inlineDecls));
    CHECK(inlineDecls.size() == 1);
    REQUIRE(inlineSink.all().size() == 1);
    CHECK(inlineSink.all()[0].message.find("can only be declared in a stylesheet rule") != std::string::npos);

    compile::DiagnosticSink sink;
    compile::RuleSet rules;
    CHECK(compile::parseXts(":root { --a: 1px; color: red; }", "t.xts", 1, sink, rules));
    CHECK(rules.rules.empty());
    REQUIRE(rules.tokens.size() == 1);
    CHECK(rules.tokens[0].name == "--a");
    CHECK(rules.tokens[0].value == "1px");
    REQUIRE(sink.all().size() == 1);
    CHECK(sink.all()[0].message.find(":root") != std::string::npos);

    compile::DiagnosticSink sink2;
    compile::RuleSet rules2;
    CHECK(!compile::parseXts("div:root { --a: 1px; }", "t.xts", 1, sink2, rules2));
}

#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-173: the descendant-combinator fix and the `:not()`, `:nth-child()`, `:first-child`,
/// `:last-child`, `[attr]`, `:focus-within` and `:lang()` selectors -- parsing, specificity and
/// runtime matching. See docs/design/05-style-and-css.md §5.3, §5.7, §5.9.2, docs/tasks/T-173.md.

using namespace xt;
using compile::Selector;
using compile::SelectorCondKind;
using compile::SelectorPart;

namespace {

/// Parses `css` (one rule) and returns its first selector; fails the test on a parse error.
Selector parseOne(const std::string& css) {
    compile::DiagnosticSink sink;
    compile::RuleSet rs;
    bool ok = compile::parseXts(css, "t.xts", 1, sink, rs);
    for (const compile::Diagnostic& d : sink.all()) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(ok);
    REQUIRE(rs.rules.size() == 1);
    REQUIRE_FALSE(rs.rules[0].selectors.empty());
    return rs.rules[0].selectors[0];
}

/// True when `css` fails to parse with an error mentioning `needle`.
bool parseFails(const std::string& css, const std::string& needle) {
    compile::DiagnosticSink sink;
    compile::RuleSet rs;
    bool ok = compile::parseXts(css, "t.xts", 1, sink, rs);
    bool found = false;
    for (const compile::Diagnostic& d : sink.all()) {
        found = found ||
                (d.level == compile::Diagnostic::Level::Error && d.message.find(needle) != std::string::npos);
    }
    return !ok && found;
}

constexpr uint32_t spec(uint32_t ids, uint32_t classes, uint32_t tags) {
    return (ids << 16) | (classes << 8) | tags;
}

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"sel.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "sel.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// One Context + one loaded Document, updated once.
struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Loaded(const std::string& css, const std::string& body) : ir(compileDoc(css, body)) {
        ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.update(0.0f);
    }

    ElementRef el(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref;
    }
    Color color(const char* id) { return doc->arena().element(el(id).node).style.base[prop::Color].c; }
    bool red(const char* id) { return color(id) == Color{1, 0, 0, 1}; }
    void update() { ctx.update(0.0f); }
};

}  // namespace

// -- Parsing ------------------------------------------------------------------------------------

TEST_CASE("T-173: whitespace is the descendant combinator whatever the next compound starts with") {
    const char* cases[] = {".a .b{color:red}",      ".a #b{color:red}",        ".a :hover{color:red}",
                           ".a [x]{color:red}",     ".a *{color:red}",         ".a div{color:red}",
                           ":hover #Bg{color:red}", ".a /* c */ .b{color:red}"};
    for (const char* css : cases) {
        CAPTURE(css);
        Selector sel = parseOne(css);
        REQUIRE(sel.parts.size() == 2);
        CHECK(sel.parts[1].combinator == 0);
    }
    Selector hoverBg = parseOne(":hover #Bg{color:red}");
    CHECK(hoverBg.parts[0].id == "Bg");
    CHECK(hoverBg.parts[0].stateMask == 0);
    CHECK(hoverBg.parts[1].stateMask == kStateHover);
}

TEST_CASE("T-173: a compound without whitespace stays one part; > is the child combinator") {
    Selector one = parseOne(".a.b:hover{color:red}");
    REQUIRE(one.parts.size() == 1);
    CHECK(one.parts[0].classes.size() == 2);
    CHECK(one.parts[0].stateMask == kStateHover);

    Selector divBg = parseOne("div#Bg{color:red}");
    REQUIRE(divBg.parts.size() == 1);
    CHECK(divBg.parts[0].tag == "div");
    CHECK(divBg.parts[0].id == "Bg");

    for (const char* css : {".a > .b{color:red}", ".a>.b{color:red}", ".a >.b{color:red}"}) {
        CAPTURE(css);
        Selector sel = parseOne(css);
        REQUIRE(sel.parts.size() == 2);
        CHECK(sel.parts[1].combinator == 1);
    }
    Selector mixed = parseOne("div.panel#main:hover > .child span{color:red}");
    REQUIRE(mixed.parts.size() == 3);
    CHECK(mixed.parts[0].tag == "span");
    CHECK(mixed.parts[1].combinator == 0);
    CHECK(mixed.parts[2].combinator == 1);
}

TEST_CASE("T-173: :not() with a simple selector list; specificity is its most specific argument") {
    Selector sel = parseOne("div:not(#a, .b, :hover){color:red}");
    REQUIRE(sel.parts.size() == 1);
    REQUIRE(sel.parts[0].conds.size() == 3);
    CHECK(sel.parts[0].conds[0].kind == SelectorCondKind::Id);
    CHECK(sel.parts[0].conds[1].kind == SelectorCondKind::Class);
    CHECK(sel.parts[0].conds[2].kind == SelectorCondKind::State);
    CHECK(sel.parts[0].conds[2].state == kStateHover);
    for (const auto& c : sel.parts[0].conds) {
        CHECK(c.negated);
        CHECK(c.notGroup == 1);
    }
    CHECK(sel.specificity == spec(1, 0, 1));
    CHECK(parseOne(".x:not(.b):not(p){color:red}").specificity == spec(0, 2, 1));
    CHECK(parseOne(":not(*){color:red}").specificity == 0);

    CHECK(parseFails(":not(:not(.a)){color:red}", "not allowed inside :not()"));
    CHECK(parseFails(":not(.a .b){color:red}", "simple selectors only"));
    CHECK(parseFails(":not(.a.b){color:red}", "simple selectors only"));
}

TEST_CASE("T-173: :nth-child() argument forms, :first-child, :last-child") {
    struct Case {
        const char* arg;
        int32_t a, b;
    };
    const Case cases[] = {{"odd", 2, 1},    {"even", 2, 0},  {"3", 0, 3},       {"2n+1", 2, 1},
                          {"2n + 1", 2, 1}, {"2n+ 1", 2, 1}, {"2n - 1", 2, -1}, {"2n-1", 2, -1},
                          {"-n+3", -1, 3},  {"n", 1, 0},     {"-2n+1", -2, 1},  {" EVEN ", 2, 0},
                          {"+n+2", 1, 2}};
    for (const Case& c : cases) {
        CAPTURE(c.arg);
        Selector sel = parseOne(std::string("li:nth-child(") + c.arg + "){color:red}");
        REQUIRE(sel.parts[0].conds.size() == 1);
        CHECK(sel.parts[0].conds[0].kind == SelectorCondKind::NthChild);
        CHECK(sel.parts[0].conds[0].a == c.a);
        CHECK(sel.parts[0].conds[0].b == c.b);
        CHECK(sel.specificity == spec(0, 1, 1));
    }
    CHECK(parseFails("li:nth-child(2x){color:red}", "invalid :nth-child()"));
    CHECK(parseFails("li:nth-child(){color:red}", "invalid :nth-child()"));

    Selector fl = parseOne("li:first-child:last-child{color:red}");
    REQUIRE(fl.parts[0].conds.size() == 2);
    CHECK(fl.parts[0].conds[0].kind == SelectorCondKind::FirstChild);
    CHECK(fl.parts[0].conds[1].kind == SelectorCondKind::LastChild);
    CHECK(fl.specificity == spec(0, 2, 1));
}

TEST_CASE("T-173: attribute selectors count as classes; [class] is rejected") {
    struct Case {
        const char* css;
        SelectorCondKind kind;
        const char* name;
        const char* value;
    };
    const Case cases[] = {
        {"[data-x]{color:red}", SelectorCondKind::AttrExists, "data-x", ""},
        {"[data-x=foo]{color:red}", SelectorCondKind::AttrEquals, "data-x", "foo"},
        {"[data-x=\"a b\"]{color:red}", SelectorCondKind::AttrEquals, "data-x", "a b"},
        {"[data-x^=pre]{color:red}", SelectorCondKind::AttrPrefix, "data-x", "pre"},
        {"[data-x$=suf]{color:red}", SelectorCondKind::AttrSuffix, "data-x", "suf"},
        {"[data-x*=mid]{color:red}", SelectorCondKind::AttrSubstring, "data-x", "mid"},
        {"[tabindex=0]{color:red}", SelectorCondKind::AttrEquals, "tabindex", "0"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.css);
        Selector sel = parseOne(c.css);
        REQUIRE(sel.parts[0].conds.size() == 1);
        CHECK(sel.parts[0].conds[0].kind == c.kind);
        CHECK(sel.parts[0].conds[0].name == c.name);
        CHECK(sel.parts[0].conds[0].value == c.value);
        CHECK(sel.specificity == spec(0, 1, 0));
    }
    CHECK(parseOne("div.a[data-x][data-y=1]{color:red}").specificity == spec(0, 3, 1));
    CHECK(parseFails("[class=a]{color:red}", "use .class"));
    CHECK(parseFails("[data-x~=a]{color:red}", "unsupported attribute selector operator"));
}

TEST_CASE("T-173: :focus-within and :lang() parse as pseudo-classes") {
    Selector fw = parseOne(".panel:focus-within .label{color:red}");
    REQUIRE(fw.parts.size() == 2);
    REQUIRE(fw.parts[1].conds.size() == 1);
    CHECK(fw.parts[1].conds[0].kind == SelectorCondKind::FocusWithin);
    CHECK(fw.specificity == spec(0, 3, 0));

    Selector lang = parseOne("p:lang(zh-TW){color:red}");
    REQUIRE(lang.parts[0].conds.size() == 1);
    CHECK(lang.parts[0].conds[0].kind == SelectorCondKind::Lang);
    CHECK(lang.parts[0].conds[0].name == "zh-TW");
    CHECK(lang.specificity == spec(0, 1, 1));
    CHECK(parseFails("p:lang(){color:red}", ":lang()"));
    CHECK(parseFails("p:bogus{color:red}", "unknown pseudo-class"));
}

// -- Runtime matching ---------------------------------------------------------------------------

TEST_CASE("T-173: .a .b needs .b inside .a, not both classes on one element") {
    Loaded l(".a .b{color:#f00}",
             R"(<div class="a"><div id="inner" class="b"></div></div><div id="both" class="a b"></div>)");
    CHECK(l.red("inner"));
    CHECK_FALSE(l.red("both"));
}

TEST_CASE("T-173: #p:checked #c (pseudo-class, then an id compound) re-resolves on a state change") {
    Loaded l("#p:checked #c{color:#f00}", R"(<div id="p"><div><div id="c"></div></div></div>)");
    CHECK_FALSE(l.red("c"));
    l.el("p").setState(kStateChecked, true);
    l.update();
    CHECK(l.red("c"));
}

TEST_CASE("T-173: :nth-child(even), :first-child, :last-child follow sibling insertion and removal") {
    Loaded l(
        "li:nth-child(even){color:#f00} li:first-child{background-color:#00f} "
        "li:last-child{border-color:#0f0} li:nth-child(2) span{color:#f00}",
        R"(<ul><li id="i1"></li><li id="i2"><span id="s2"></span></li><li id="i3"></li><li id="i4"></li></ul>)");
    CHECK_FALSE(l.red("i1"));
    CHECK(l.red("i2"));
    CHECK_FALSE(l.red("i3"));
    CHECK(l.red("i4"));
    CHECK(l.red("s2"));
    const Color blue{0, 0, 1, 1};
    const Color green{0, 1, 0, 1};
    auto bg = [&](const char* id) {
        return l.doc->arena().element(l.el(id).node).style.base[prop::BackgroundColor].c;
    };
    auto border = [&](const char* id) {
        return l.doc->arena().element(l.el(id).node).style.base[prop::BorderColor].c;
    };
    CHECK(bg("i1") == blue);
    CHECK_FALSE(bg("i2") == blue);
    CHECK(border("i4") == green);
    CHECK_FALSE(border("i3") == green);

    // Removing the first item shifts every position (and the ancestor-compound rule's subtree).
    l.doc->remove(l.el("i1"));
    l.update();
    CHECK_FALSE(l.red("i2"));
    CHECK_FALSE(l.red("s2"));
    CHECK(l.red("i3"));
    CHECK_FALSE(l.red("i4"));
    CHECK(bg("i2") == blue);

    // Appending a new last item moves :last-child off i4.
    ElementRef li = l.doc->createElement("li");
    l.doc->appendChild(l.doc->byId("i4").parent(), li);
    l.update();
    CHECK_FALSE(border("i4") == green);
    CHECK(l.doc->arena().element(li.node).style.base[prop::BorderColor].c == green);
    CHECK(l.doc->arena().element(li.node).style.base[prop::Color].c == Color{1, 0, 0, 1});
}

TEST_CASE("T-173: :nth-child an+b forms match the right positions") {
    std::string body = "<div>";
    for (int i = 1; i <= 7; ++i) {
        body += "<p id=\"p" + std::to_string(i) + "\"></p>";
    }
    body += "</div>";
    struct Case {
        const char* arg;
        std::vector<int> hits;
    };
    const Case cases[] = {{"odd", {1, 3, 5, 7}},  {"3n+1", {1, 4, 7}}, {"-n+3", {1, 2, 3}},
                          {"2n-1", {1, 3, 5, 7}}, {"5", {5}},          {"n+6", {6, 7}}};
    for (const Case& c : cases) {
        CAPTURE(c.arg);
        Loaded l(std::string("p:nth-child(") + c.arg + "){color:#f00}", body);
        for (int i = 1; i <= 7; ++i) {
            CAPTURE(i);
            const std::string id = "p" + std::to_string(i);
            bool want = std::find(c.hits.begin(), c.hits.end(), i) != c.hits.end();
            CHECK(l.red(id.c_str()) == want);
        }
    }
}

TEST_CASE("T-173: :not() excludes, and its specificity beats a longer class chain") {
    Loaded l("div:not(.x, #skip){color:#f00} .b.c.d{color:#00f}",
             R"(<div id="plain"></div><div id="x" class="x"></div><div id="skip"></div>)"
             R"(<div id="chain" class="b c d"></div>)");
    CHECK(l.red("plain"));
    CHECK_FALSE(l.red("x"));
    CHECK_FALSE(l.red("skip"));
    // div:not(#skip) is (1,0,1) > .b.c.d (0,3,0).
    CHECK(l.red("chain"));
}

TEST_CASE("T-173: attribute selectors match .xt attributes, data-* included, and follow setAttribute") {
    Loaded l(
        "[data-kind]{color:#f00} [data-kind=hot]{background-color:#00f} [data-name^=btn-]{opacity:0.5} "
        "[data-name$=-ok]{opacity:0.25} [data-name*=mid]{border-color:#0f0} [id=byid]{z-index:3}",
        R"(<div id="a" data-kind="cold" data-name="btn-ok"></div><div id="b" data-kind="hot" data-name="xmidx"></div>)"
        R"(<div id="c"></div><div id="byid"></div>)");
    const Color blue{0, 0, 1, 1};
    const Color green{0, 1, 0, 1};
    auto st = [&](const char* id) -> const ComputedStyle& {
        return l.doc->arena().element(l.el(id).node).style;
    };
    CHECK(l.red("a"));
    CHECK(l.red("b"));
    CHECK_FALSE(l.red("c"));
    CHECK_FALSE(st("a").base[prop::BackgroundColor].c == blue);
    CHECK(st("b").base[prop::BackgroundColor].c == blue);
    CHECK(st("a").base[prop::Opacity].f == doctest::Approx(0.25f));  // Both match; the later rule wins.
    CHECK(st("b").base[prop::Opacity].f == doctest::Approx(1.0f));
    CHECK(st("b").base[prop::BorderColor].c == green);
    CHECK(st("byid").zIndex == doctest::Approx(3.0f));

    REQUIRE(l.el("c").setAttribute("data-kind", "hot") == Status::kOk);
    REQUIRE(l.el("c").setAttribute("data-name", "btn-go") == Status::kOk);  // A runtime string.
    l.update();
    CHECK(l.red("c"));
    CHECK(st("c").base[prop::BackgroundColor].c == blue);
    CHECK(st("c").base[prop::Opacity].f == doctest::Approx(0.5f));
}

TEST_CASE("T-173: :focus-within marks the ancestor chain, and a deep rule reaches siblings") {
    Loaded l(".panel:focus-within{color:#f00} .panel:focus-within .label{color:#f00}",
             R"(<div id="panel" class="panel"><div id="label" class="label"></div>)"
             R"(<div id="wrap"><input id="field"/></div></div><div id="other" class="panel"></div>)");
    CHECK_FALSE(l.red("panel"));
    CHECK_FALSE(l.red("label"));
    l.doc->setFocus(l.el("field"));
    l.update();
    CHECK(l.red("panel"));
    CHECK(l.red("label"));
    CHECK_FALSE(l.red("other"));
    l.doc->setFocus(ElementRef{});
    l.update();
    CHECK_FALSE(l.red("panel"));
    CHECK_FALSE(l.red("label"));
}

TEST_CASE("T-173: :lang() uses the inherited lang attribute with BCP 47 prefix matching") {
    Loaded l(
        ":lang(en){color:#f00} p:lang(zh){background-color:#00f}",
        R"(<div lang="en-US"><p id="en"></p></div><div lang="EN"><p id="upper"></p></div>)"
        R"(<div lang="eng"><p id="eng"></p></div><div lang="zh-Hant-TW"><p id="zh"></p></div><p id="none"></p>)");
    CHECK(l.red("en"));
    CHECK(l.red("upper"));
    CHECK_FALSE(l.red("eng"));
    CHECK_FALSE(l.red("none"));
    CHECK(l.doc->arena().element(l.el("zh").node).style.base[prop::BackgroundColor].c == Color{0, 0, 1, 1});
}

TEST_CASE("T-173: IR validation of condition parts") {
    auto openWith = [](std::vector<IrSelPart> parts) {
        compile::IrWriter w;
        StringId name = w.intern("data-x");
        compile::StyleBuilder style;
        style.selectorParts = std::move(parts);
        IrRule rule{};
        rule.selOffset = 0;
        rule.selCount = static_cast<uint32_t>(style.selectorParts.size());
        style.rules.push_back(rule);
        for (IrSelPart& p : style.selectorParts) {
            if (p.tag == 0xFFFFFFFEu) {
                p.tag = name;  // Placeholder for the interned attribute name.
            }
        }
        w.setStyle(std::move(style));
        std::vector<uint8_t> bytes = w.finish();
        return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true}).ok();
    };
    IrSelPart subject{};
    subject.tag = kInvalidString;
    subject.id = kInvalidString;
    IrSelPart cond{};
    cond.combinator = kIrSelCondition;
    cond.classCount = static_cast<uint8_t>(IrSelCond::AttrExists);
    cond.tag = 0xFFFFFFFEu;
    cond.id = kInvalidString;
    IrSelPart nth{};
    nth.combinator = kIrSelCondition;
    nth.classCount = static_cast<uint8_t>(IrSelCond::NthChild) | kIrSelCondNegated;
    nth.tag = static_cast<uint32_t>(-2);  // a = -2: not a StringId, still valid for NthChild.
    nth.id = 7;

    CHECK(openWith({subject, cond, nth}));
    CHECK_FALSE(openWith({cond, subject}));  // A chain cannot start with a condition.
    IrSelPart badCode = cond;
    badCode.classCount = kIrSelCondLast + 1;  // T-174 made 16 (CustomState) valid.
    CHECK_FALSE(openWith({subject, badCode}));
    IrSelPart badCombinator = subject;
    badCombinator.combinator = 5;  // T-218 made 4 (kIrSelSceneTarget) valid.
    CHECK_FALSE(openWith({subject, badCombinator}));
    // T-218: a scene target -- Any carries no name, Name needs one, and only right after the subject.
    IrSelPart anyTarget{};
    anyTarget.combinator = kIrSelSceneTarget;
    anyTarget.classCount = static_cast<uint8_t>(IrSceneTarget::Any);
    anyTarget.tag = kInvalidString;
    anyTarget.id = kInvalidString;
    CHECK(openWith({subject, anyTarget, cond}));
    CHECK_FALSE(openWith({anyTarget, subject}));
    CHECK_FALSE(openWith({subject, cond, anyTarget}));
    IrSelPart unnamed = anyTarget;
    unnamed.classCount = static_cast<uint8_t>(IrSceneTarget::Name);
    CHECK_FALSE(openWith({subject, unnamed}));
    IrSelPart badString = cond;
    badString.tag = 12345;
    CHECK_FALSE(openWith({subject, badString}));
}

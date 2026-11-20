#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/expressions.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-740 (Docs/design/56-symbol-identity-style-scope.md §56.3 items 0, 2, 3): a symbol's identity
/// is its path under `shared/symbols` -- `<ns>/<Name>` for a file in a namespace directory, `Name`
/// for a flat one. Two libraries may both ship a `Button`; a bare name inside a namespaced file
/// resolves in its own namespace first; a bare name anywhere else names only the flat symbol.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr std::string_view kFlatButton = R"(<xt><head>
  <symbol name="Button"><div id="Bg"></div></symbol>
  <style>
    symbol(Button) {
      #Bg { width: 10px; height: 10px; background-color: #ff0000; }
      @timeline main { duration: 1s; #Bg { 0s { opacity: 0 } 1s { opacity: 1 } } }
    }
  </style>
</head></xt>)";

constexpr std::string_view kSurveyButton = R"(<xt><head>
  <symbol name="Button"><div id="Bg"></div></symbol>
  <style>
    symbol(Button) {
      #Bg { width: 20px; height: 10px; background-color: #00ff00; }
      @timeline main { duration: 1s; #Bg { 0s { opacity: 1 } 1s { opacity: 0 } } }
    }
  </style>
</head></xt>)";

constexpr const char* kFlatHref = "shared/symbols/Button.xt";
constexpr const char* kSurveyHref = "shared/symbols/survey-2026/Button.xt";

std::string doc(std::string_view hrefs, std::string_view body, std::string_view style = "") {
    std::string out = "<xt><head>";
    size_t start = 0;
    while (start < hrefs.size()) {
        size_t end = hrefs.find(' ', start);
        end = end == std::string_view::npos ? hrefs.size() : end;
        out += "<link rel=\"symbols\" href=\"" + std::string(hrefs.substr(start, end - start)) + "\"/>";
        start = end + 1;
    }
    out += "<style>" + std::string(style) + "</style></head><body id=\"Root\">" + std::string(body) +
           "</body></xt>";
    return out;
}

CompileResult compileFiles(std::vector<SourceFile> files, CompileOptions options = {}) {
    options.emitMeta = false;
    if (options.rootDir.empty()) {
        options.rootDir = "t740-no-such-dir";
    }
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasDiag(const CompileResult& r, Diagnostic::Level level, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void requireOk(const CompileResult& r) { test::requireCompiled(r); }

struct Loaded {
    std::unique_ptr<Document> doc;

    explicit Loaded(const CompileResult& r) {
        requireOk(r);
        // Copied: `r` may be a temporary.
        REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, true, nullptr, nullptr, doc) == Status::kOk);
        doc->setViewport(Rect{0, 0, 200, 100});
        doc->update(0.0f);
    }
    Color bg(std::string_view id) const { return doc->byId(id).computedStyleValue(prop::BackgroundColor).c; }
    float width(std::string_view id) const { return doc->byId(id).layoutBox().width; }
    float opacity(std::string_view id) const { return doc->byId(id).property(prop::Opacity).f; }
};

bool isRed(Color c) { return c.r > 0.99f && c.g < 0.01f; }
bool isGreen(Color c) { return c.g > 0.99f && c.r < 0.01f; }

std::vector<std::string> symbNames(const CompileResult& r) {
    std::vector<std::string> out;
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    for (const IrSymbolInstance& inst : view.value().symbols().instances()) {
        out.emplace_back(view.value().string(inst.symbol));
    }
    return out;
}

}  // namespace

TEST_CASE("T-740 two libraries both ship Button: each instance gets its own rules and timeline") {
    const std::string main = doc(std::string(kFlatHref) + " " + kSurveyHref,
                                 R"(<use symbol="Button" id="A"/><use symbol="survey-2026/Button" id="B"/>)");
    const CompileResult r =
        compileFiles({{"main.xt", main}, {kFlatHref, kFlatButton}, {kSurveyHref, kSurveyButton}});
    Loaded l(r);
    CHECK(isRed(l.bg("A/Bg")));
    CHECK(isGreen(l.bg("B/Bg")));
    CHECK(l.width("A/Bg") == doctest::Approx(10.0f));
    CHECK(l.width("B/Bg") == doctest::Approx(20.0f));
    // One `main` per instance, each the one of its own symbol.
    REQUIRE(l.doc->seekTimeline("A", "main", 0.25f) == Status::kOk);
    REQUIRE(l.doc->seekTimeline("B", "main", 0.25f) == Status::kOk);
    l.doc->update(0.0f);
    CHECK(l.opacity("A/Bg") == doctest::Approx(0.25f));
    CHECK(l.opacity("B/Bg") == doctest::Approx(0.75f));
    // SYMB records the qualified name as it is.
    CHECK(symbNames(r) == std::vector<std::string>{"Button", "survey-2026/Button"});
}

TEST_CASE("T-740 a bare name inside a namespaced file resolves in its namespace first, then flat") {
    const std::string dialog = R"(<xt><head>
      <symbol name="Dialog"><div id="Box"><use symbol="Button" id="Ok"/><use symbol="Icon" id="I"/></div></symbol>
    </head></xt>)";
    const std::string icon = R"(<xt><head>
      <symbol name="Icon"><div id="Glyph"></div></symbol>
      <style>symbol(Icon) { #Glyph { width: 7px; height: 7px; } }</style>
    </head></xt>)";
    // A flat file's bare `Button` is the flat one, even with survey-2026/Button present.
    const std::string card = R"(<xt><head>
      <symbol name="Card"><div id="Box"><use symbol="Button" id="Btn"/></div></symbol>
    </head></xt>)";
    const std::string main = doc(std::string(kFlatHref) + " " + kSurveyHref +
                                     " shared/symbols/survey-2026/Dialog.xt shared/symbols/Icon.xt "
                                     "shared/symbols/Card.xt",
                                 R"(<use symbol="survey-2026/Dialog" id="D"/><use symbol="Card" id="C"/>)");
    const CompileResult r = compileFiles({{"main.xt", main},
                                          {kFlatHref, kFlatButton},
                                          {kSurveyHref, kSurveyButton},
                                          {"shared/symbols/survey-2026/Dialog.xt", dialog},
                                          {"shared/symbols/Icon.xt", icon},
                                          {"shared/symbols/Card.xt", card}});
    Loaded l(r);
    CHECK(isGreen(l.bg("D/Ok/Bg")));                       // survey-2026/Button
    CHECK(l.width("D/I/Glyph") == doctest::Approx(7.0f));  // No survey-2026/Icon: the flat Icon.
    CHECK(isRed(l.bg("C/Btn/Bg")));                        // Flat file: the flat Button.
    const std::vector<std::string> names = symbNames(r);
    CHECK(std::find(names.begin(), names.end(), "survey-2026/Dialog") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Icon") != names.end());
}

TEST_CASE("T-740 a bare name at document level never reaches a namespaced symbol") {
    const CompileResult r = compileFiles(
        {{"main.xt", doc(kSurveyHref, R"(<use symbol="Button" id="A"/>)")}, {kSurveyHref, kSurveyButton}});
    CHECK_FALSE(r.ok());
    CHECK(hasDiag(r, Diagnostic::Level::Error,
                  "unknown symbol \"Button\"; a bare name only names a flat symbol here, not "
                  "\"survey-2026/Button\""));
    // Inside a namespaced file, both names tried are listed.
    const std::string dialog = R"(<xt><head>
      <symbol name="Dialog"><div id="Box"><use symbol="Missing" id="M"/></div></symbol></head></xt>)";
    const CompileResult inner =
        compileFiles({{"main.xt", doc("shared/symbols/survey-2026/Dialog.xt",
                                      R"(<use symbol="survey-2026/Dialog" id="D"/>)")},
                      {"shared/symbols/survey-2026/Dialog.xt", dialog}});
    CHECK(hasDiag(inner, Diagnostic::Level::Error,
                  "symbol \"survey-2026/Dialog\" uses unknown symbol \"Missing\" (tried "
                  "\"survey-2026/Missing\" and \"Missing\")"));
}

TEST_CASE("T-740 .xts symbol(ns/Name){} parses one '/' and styles only that symbol") {
    DiagnosticSink sink;
    RuleSet rules;
    REQUIRE(parseXts(
        "symbol(survey-2026/Button) { #Bg { opacity: 0.5; } } symbol(Button) { #Bg { opacity: 1; } }",
        "t.xts", 1, sink, rules));
    REQUIRE(rules.rules.size() == 2);
    CHECK(rules.rules[0].symbol == "survey-2026/Button");
    CHECK(rules.rules[1].symbol == "Button");
    DiagnosticSink bad;
    RuleSet badRules;
    CHECK_FALSE(parseXts("symbol(survey-2026/) { #Bg { opacity: 0.5; } }", "t.xts", 1, bad, badRules));
    CHECK(bad.hasErrors());

    const std::string main = doc(std::string(kFlatHref) + " " + kSurveyHref,
                                 R"(<use symbol="Button" id="A"/><use symbol="survey-2026/Button" id="B"/>)",
                                 "symbol(survey-2026/Button) { #Bg { background-color: #0000ff; } }");
    Loaded l(compileFiles({{"main.xt", main}, {kFlatHref, kFlatButton}, {kSurveyHref, kSurveyButton}}));
    CHECK(isRed(l.bg("A/Bg")));
    const Color b = l.bg("B/Bg");
    CHECK(b.b > 0.99f);
    CHECK(b.g < 0.01f);
}

TEST_CASE("T-740 errors: unknown qualified name, duplicate qualified name, invalid namespace folder") {
    const CompileResult unknown =
        compileFiles({{"main.xt", doc(kSurveyHref, R"(<use symbol="other/Button" id="A"/>)")},
                      {kSurveyHref, kSurveyButton}});
    CHECK(hasDiag(unknown, Diagnostic::Level::Error, "unknown symbol \"other/Button\""));

    // Two files of one namespace defining Button: a duplicate, naming both files.
    const std::string extra = R"(<xt><head><symbol name="Button"><div id="X"></div></symbol></head></xt>)";
    const CompileResult dup =
        compileFiles({{"main.xt", doc(std::string(kSurveyHref) + " shared/symbols/survey-2026/Extra.xt",
                                      R"(<use symbol="survey-2026/Button" id="A"/>)")},
                      {kSurveyHref, kSurveyButton},
                      {"shared/symbols/survey-2026/Extra.xt", extra}});
    CHECK(hasDiag(dup, Diagnostic::Level::Error,
                  "duplicate symbol \"survey-2026/Button\" (also defined in "
                  "\"shared/symbols/survey-2026/Button.xt\")"));
    // The same name flat and namespaced is not a duplicate.
    requireOk(compileFiles(
        {{"main.xt", doc(std::string(kFlatHref) + " " + kSurveyHref, R"(<use symbol="Button" id="A"/>)")},
         {kFlatHref, kFlatButton},
         {kSurveyHref, kSurveyButton}}));

    // A folder that is not a namespace: a warning, and the file is flat.
    const CompileResult invalid = compileFiles(
        {{"main.xt", doc("shared/symbols/Survey_2026/Button.xt", R"(<use symbol="Button" id="A"/>)")},
         {"shared/symbols/Survey_2026/Button.xt", kSurveyButton}});
    requireOk(invalid);
    CHECK(hasDiag(invalid, Diagnostic::Level::Warning,
                  "symbol library folder \"Survey_2026\" is not a valid namespace"));
    CHECK(symbNames(invalid) == std::vector<std::string>{"Button"});
}

TEST_CASE("T-740 CompileOptions::symbolsDir names the project's symbol directory") {
    // A project whose shared.symbols is `lib/syms`: the folder rule follows it, not the name.
    CompileOptions options;
    options.rootDir = "proj";
    options.xtDir = "proj/ui/main";
    options.symbolsDir = "lib/syms";
    const std::string href = "../../lib/syms/survey-2026/Button.xt";
    const CompileResult r = compileFiles(
        {{"main.xt", doc(href, R"(<use symbol="survey-2026/Button" id="B"/>)")}, {href, kSurveyButton}},
        options);
    Loaded l(r);
    CHECK(isGreen(l.bg("B/Bg")));
    // A `symbols/<dir>/` path outside that directory is flat when the option is set.
    const std::string elsewhere = "../../symbols/survey-2026/Button.xt";
    const CompileResult flat = compileFiles(
        {{"main.xt", doc(elsewhere, R"(<use symbol="Button" id="B"/>)")}, {elsewhere, kSurveyButton}},
        options);
    requireOk(flat);
    CHECK(symbNames(flat) == std::vector<std::string>{"Button"});
}

TEST_CASE("T-740 spawn() of a qualified symbol and a namespaced symbol's expressions") {
    const CompileResult spawned = compileFiles(
        {{"main.xt",
          doc(kSurveyHref, R"X(<div id="Host" on:click="spawn(survey-2026/Button, lifetime=1s)"></div>)X")},
         {kSurveyHref, kSurveyButton}});
    requireOk(spawned);

    const std::string card = R"X(<xt version="2">
  <symbol name="Card">
    <div id="Box"><div id="B"></div></div>
    <timelines>
      @timeline pop { duration: 2s; #B { 0s { opacity: 0 } 1s { opacity: 1 } } }
    </timelines>
    <expressions>
      pop #B opacity = 0.5 + time * 0.0
    </expressions>
  </symbol>
</xt>)X";
    const CompileResult r = compileFiles({{"main.xt", doc("shared/symbols/survey-2026/Card.xt",
                                                          R"(<use symbol="survey-2026/Card" id="C1"/>)")},
                                          {"shared/symbols/survey-2026/Card.xt", card}});
    Loaded l(r);
    REQUIRE(r.expressions.size() == 1);
    CHECK(r.expressions[0].symbol == "survey-2026/Card");
    CHECK(r.expressions[0].scopePath == "C1");
    CHECK(r.expressions[0].status == ExpressionOutcome::Status::Lowered);
    REQUIRE(l.doc->seekTimeline("C1", "pop", 1.0f) == Status::kOk);
    l.doc->update(0.0f);
    CHECK(l.opacity("C1/B") == doctest::Approx(0.5f));
}

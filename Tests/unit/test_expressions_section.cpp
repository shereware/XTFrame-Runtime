#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "expr/expr.h"
#include "expressions_section.h"
#include "xt/compile/canonical_writer.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt_v2_test_util.h"

/// T-767 (Docs/tasks/T-767.md, Docs/design/58-value-functions-and-drivers.md §58.4): the
/// `<expressions>` section -- its grammar, the lowering into `@timeline` tracks checked against the
/// expression engine through runtime playback, the `expr.*` notes (each one skips only its entry),
/// one component of a vector row, symbols, `--strict`, the canonical writer and the IR size.

using namespace xt;
using namespace xt::compile;
using xt::test::v2::compileFiles;
using xt::test::v2::findCode;
using xt::test::v2::requireCompiled;

namespace xe = xt::compile::expr;

namespace {

struct LoadedDoc {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    float clock = 0;

    explicit LoadedDoc(const std::vector<uint8_t>& ir) {
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.update(0.0f);
    }

    /// `prop` of `id` with timeline `name` seeked to `t`.
    float at(const char* name, float t, const char* id, PropId prop) {
        REQUIRE(doc->seekTimeline("", name, t) == Status::kOk);
        clock += 0.01f;
        ctx.update(clock);
        return doc->byId(id).property(prop).f;
    }
};

constexpr const char* kBody = R"X(
  <body>
    <div id="Row">
      <div id="Logo"></div>
      <div id="Gear"></div>
      <div id="Dot1"></div>
      <div id="Dot2"></div>
      <div id="Dot3"></div>
    </div>
  </body>
)X";

constexpr const char* kTimelines = R"X(
  <timelines>
    @timeline intro {
      duration: 4s;
      #Logo { 0s { translate: 0px 0px } 2s { translate: 10px 20px } }
      #Gear { 0s { rotate: 0deg } 1s { rotate: 30deg } }
      #Dot2 { 0s { opacity: 0 } 1s { opacity: 1 } }
      #Dot3 { 0s { opacity: 0 } 1s { opacity: 1 } }
    }
  </timelines>
)X";

std::string docWith(const std::string& expressions, const std::string& timelines = kTimelines) {
    return std::string("<xt version=\"2\">") + kBody + timelines + expressions + "</xt>\n";
}

CompileResult compileDoc(const std::string& text, CompileOptions options = {}) {
    return compileFiles({SourceFile{"e.xt", text}}, options);
}

/// The 1-based line of the first occurrence of `needle` in `text`.
int lineOf(const std::string& text, const std::string& needle) {
    const size_t pos = text.find(needle);
    REQUIRE(pos != std::string::npos);
    int line = 1;
    for (size_t i = 0; i < pos; ++i) {
        line += text[i] == '\n' ? 1 : 0;
    }
    return line;
}

size_t countCode(const std::vector<Diagnostic>& diags, const std::string& code) {
    size_t n = 0;
    for (const Diagnostic& d : diags) {
        n += d.code == code ? size_t{1} : size_t{0};
    }
    return n;
}

const ExpressionOutcome* outcomeFor(const CompileResult& r, const std::string& element,
                                    const std::string& row) {
    for (const ExpressionOutcome& o : r.expressions) {
        if (o.element == element && o.property == row) {
            return &o;
        }
    }
    return nullptr;
}

/// Direct evaluation of `text` by the engine on a row of `keys` (what the compiler lowers from).
struct Direct {
    std::vector<xe::ExprKey> keys;
    xe::Program program;
    xe::ExprContext ctx;

    Direct(const std::string& text, std::vector<xe::ExprKey> rowKeys, int index, uint32_t seed)
        : keys(std::move(rowKeys)) {
        xe::ParseResult parsed = xe::parseExpression(text);
        REQUIRE(parsed.ok());
        program = std::move(parsed.program);
        ctx.row = xe::ExprRow{keys, {}};
        ctx.duration = 4;
        ctx.index = index;
        ctx.seed = seed;
    }

    double at(double t) const {
        const xe::EvalResult e = xe::evaluateExpression(program, ctx, t);
        REQUIRE(e.status == xe::ExprStatus::Ok);
        double out = 0;
        std::vector<Diagnostic> d;
        REQUIRE(xe::componentOf(e.value, ctx.component, out, d));
        return out;
    }
};

xe::ExprKey key(double t, double v) { return xe::ExprKey{t, v, Easing::linear()}; }

}  // namespace

// ---- grammar ---------------------------------------------------------------------------------

TEST_CASE("T-767 <expressions> grammar: single-line, off, multi-line, comments, line numbers") {
    const std::string text =
        "\n"
        "  // shake it\n"
        "  intro #Logo translate.y = wiggle(2, 6)\n"
        "\n"
        "  intro #Gear rotate off = value + time * 90\n"
        "  intro #Dot3 opacity = {\n"
        "    posterizeTime(12);\n"
        "    var s = \"}\"; // a } in a comment\n"
        "    valueAtTime(time - (index - 1) * 0.08)\n"
        "  }\n"
        "  // trailing\n";
    DiagnosticSink sink;
    std::vector<ExpressionEntry> entries;
    std::vector<std::string> trailing;
    parseExpressionsSection(text, 10, 14, "e.xt", sink, entries, &trailing);
    CHECK(sink.all().empty());
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].timeline == "intro");
    CHECK(entries[0].element == "Logo");
    CHECK(entries[0].property == "translate");
    CHECK(entries[0].component == "y");
    CHECK(entries[0].row() == "translate.y");
    CHECK_FALSE(entries[0].off);
    CHECK(entries[0].text == "wiggle(2, 6)");
    CHECK(entries[0].comments == std::vector<std::string>{"// shake it"});
    CHECK(entries[0].line == 12);
    CHECK(entries[0].col == 3);
    CHECK(entries[0].textLine == 12);
    CHECK(entries[0].textCol == 29);
    CHECK(entries[1].off);
    CHECK(entries[1].component.empty());
    CHECK(entries[1].line == 14);
    CHECK(entries[2].multiline);
    CHECK(entries[2].line == 15);
    CHECK(entries[2].textLine == 15);
    CHECK(entries[2].text.find("posterizeTime(12);") != std::string::npos);
    CHECK(entries[2].text.find("valueAtTime") != std::string::npos);
    CHECK(trailing == std::vector<std::string>{"// trailing"});
}

TEST_CASE("T-767 <expressions> grammar: a malformed entry is an expr.entry note, the others parse") {
    const std::string text =
        "  intro Logo opacity = 1\n"           // no '#'
        "  intro #Logo opacity\n"              // no '='
        "  intro #Logo translate.w = 1\n"      // bad component
        "  intro #Logo opacity maybe = 1\n"    // not `off`
        "  intro #Logo opacity =   \n"         // empty
        "  intro #Gear rotate = time * 90\n"   // fine
        "  intro #Dot3 opacity = { value \n";  // unclosed
    DiagnosticSink sink;
    std::vector<ExpressionEntry> entries;
    parseExpressionsSection(text, 1, 1, "e.xt", sink, entries);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].element == "Gear");
    CHECK(countCode(sink.all(), "expr.entry") == 6);
    for (const Diagnostic& d : sink.all()) {
        CHECK(d.level == Diagnostic::Level::Warning);
    }
    CHECK(sink.noteCount() == 6);  // Notes: --strict does not count them.
    CHECK(sink.all()[1].line == 2);
}

TEST_CASE("T-767 <expressions>: document and symbol level are kept raw; v1 rejects the section") {
    const std::string text =
        "<xt version=\"2\"><symbol name=\"Card\"><div id=\"Box\"></div>"
        "<expressions>pop #Box opacity = 1</expressions></symbol>"
        "<body><div id=\"A\"></div></body>\n<expressions>\n  t #A opacity = 0.5\n</expressions></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(text, "s.xt", sink, tree));
    REQUIRE(tree.expressionSections.size() == 2);
    CHECK(tree.expressionSections[0].symbol == "Card");
    CHECK(tree.expressionSections[0].text == "pop #Box opacity = 1");
    CHECK(tree.expressionSections[1].symbol.empty());
    CHECK(tree.expressionSections[1].lineOffset == 2);

    DiagnosticSink v1Sink;
    DomTree v1;
    CHECK_FALSE(
        parseXt("<xt><body></body><expressions>t #A opacity = 1</expressions></xt>", "v1.xt", v1Sink, v1));
    CHECK(findCode(v1Sink.all(), "expr.v1") != nullptr);
}

// ---- lowering against the engine through playback --------------------------------------------

TEST_CASE("T-767 lowered rows play back like the engine evaluates them") {
    const std::string text = docWith(R"X(
  <expressions>
    intro #Logo translate.y = wiggle(2, 6)
    intro #Gear rotate = value + time * 90
    intro #Dot3 opacity = loopOut("pingpong")
    intro #Dot2 opacity = {
      var d = (index - 1) * 0.25;
      valueAtTime(time - d)
    }
  </expressions>
)X");
    const CompileResult r = compileDoc(text);
    requireCompiled(r);
    REQUIRE(r.expressions.size() == 4);
    for (const ExpressionOutcome& o : r.expressions) {
        CAPTURE(o.element);
        CHECK(o.status == ExpressionOutcome::Status::Lowered);
        CHECK(o.diagnostics.empty());
        CHECK_FALSE(o.keys.empty());
    }
    LoadedDoc loaded(r.ir);
    const Direct logo("wiggle(2, 6)", {key(0, 0), key(2, 20)}, 1,
                      expressionSeed("intro", "Logo", "translate", "y"));
    const Direct gear("value + time * 90", {key(0, 0), key(1, 30)}, 2,
                      expressionSeed("intro", "Gear", "rotate", ""));
    const Direct dot3("loopOut(\"pingpong\")", {key(0, 0), key(1, 1)}, 5,
                      expressionSeed("intro", "Dot3", "opacity", ""));
    const Direct dot2("var d = (index - 1) * 0.25;\nvalueAtTime(time - d)", {key(0, 0), key(1, 1)}, 4,
                      expressionSeed("intro", "Dot2", "opacity", ""));
    for (int k = 0; k <= 240; k += 7) {
        const double t = k / 60.0;
        CAPTURE(t);
        CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), "Logo", prop::TranslateY) - logo.at(t)) <=
              0.25 + 1e-3);
        CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), "Gear", prop::Rotation) - gear.at(t)) <=
              0.1 + 1e-3);
        CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), "Dot3", prop::Opacity) - dot3.at(t)) <=
              0.002 + 1e-4);
        CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), "Dot2", prop::Opacity) - dot2.at(t)) <=
              0.002 + 1e-4);
    }
    // The wiggle really moves the row (not the original 0 -> 20 ramp).
    CHECK(std::fabs(loaded.at("intro", 3.0f, "Logo", prop::TranslateY) - 20.0f) > 1e-3);
}

TEST_CASE("T-767 one component replaced: the other component's keys stay as they are") {
    const CompileResult plain = compileDoc(docWith(""));
    const CompileResult withY =
        compileDoc(docWith("<expressions>intro #Logo translate.y = wiggle(2, 6)</expressions>"));
    requireCompiled(plain);
    requireCompiled(withY);
    LoadedDoc a(plain.ir);
    LoadedDoc b(withY.ir);
    for (float t : {0.0f, 0.3f, 1.0f, 1.7f, 2.0f, 3.5f}) {
        CAPTURE(t);
        CHECK(a.at("intro", t, "Logo", prop::TranslateX) ==
              doctest::Approx(b.at("intro", t, "Logo", prop::TranslateX)));
    }
    CHECK(a.at("intro", 1.0f, "Logo", prop::TranslateX) == doctest::Approx(5.0f));
}

TEST_CASE("T-767 off, and documents without the section, compile to the same IR") {
    const CompileResult plain = compileDoc(docWith(""));
    requireCompiled(plain);
    const CompileResult off =
        compileDoc(docWith("<expressions>\n  intro #Logo translate.y off = wiggle(2, 6)\n</expressions>"));
    requireCompiled(off);
    CHECK(off.ir == plain.ir);
    REQUIRE(off.expressions.size() == 1);
    CHECK(off.expressions[0].status == ExpressionOutcome::Status::Off);
    CHECK(off.expressions[0].keys.empty());
    CHECK(plain.expressions.empty());
}

// ---- notes: each one skips its entry only ----------------------------------------------------

TEST_CASE("T-767 every expr.* problem skips its entry; the timeline and the other entries compile") {
    const CompileResult plain = compileDoc(docWith(""));
    requireCompiled(plain);
    struct Case {
        const char* entry;
        const char* code;
    };
    const Case cases[] = {
        {"nope #Logo translate.y = wiggle(2, 6)", "expr.timeline"},
        {"intro #Ghost opacity = 0.5", "expr.element"},
        {"intro #Logo color = 1", "expr.property"},
        {"intro #Logo frobnicate = 1", "expr.property"},
        {"intro #Logo translate = 1", "expr.property"},
        {"intro #Logo opacity.x = 1", "expr.property"},
        {"intro #Logo translate.y = wiggle(2, ", "expr.syntax"},
        {"intro #Logo translate.y = nosuchname + 1", "expr.name"},
        {"intro #Logo translate.y = port(\"hp\") * 2", "expr.port"},  // T-777: hp is not declared
        {"intro #Logo entry-without-equals", "expr.entry"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.entry);
        const std::string text = docWith(std::string("<expressions>\n") + c.entry + "\n</expressions>");
        const CompileResult r = compileDoc(text);
        requireCompiled(r);
        const Diagnostic* d = findCode(r.diagnostics, c.code);
        REQUIRE(d != nullptr);
        CHECK(d->level == Diagnostic::Level::Warning);
        CHECK(d->line == lineOf(text, c.entry));
        CHECK(r.ir == plain.ir);  // The row and the timeline keep their keys.
    }
    // A bad entry next to a good one: the good one is still lowered.
    const CompileResult mixed = compileDoc(docWith(
        "<expressions>\n intro #Ghost opacity = 1\n intro #Gear rotate = value + time * 90\n</expressions>"));
    requireCompiled(mixed);
    REQUIRE(outcomeFor(mixed, "Ghost", "opacity") != nullptr);
    CHECK(outcomeFor(mixed, "Ghost", "opacity")->status == ExpressionOutcome::Status::Skipped);
    CHECK(outcomeFor(mixed, "Gear", "rotate")->status == ExpressionOutcome::Status::Lowered);
    CHECK(mixed.ir != plain.ir);
}

TEST_CASE("T-767 an engine diagnostic points at its place in the file") {
    const std::string text =
        docWith("<expressions>\n intro #Gear rotate = {\n   var a = 1;\n   a +* 2\n }\n</expressions>");
    const CompileResult r = compileDoc(text);
    requireCompiled(r);
    const Diagnostic* d = findCode(r.diagnostics, "expr.syntax");
    REQUIRE(d != nullptr);
    CHECK(d->line == lineOf(text, "a +* 2"));
    CHECK(d->message.find("intro #Gear rotate") != std::string::npos);
}

TEST_CASE("T-767 mixed units, duplicates, motion paths and rows without keys") {
    const std::string timelines = R"X(
  <timelines>
    @timeline intro {
      duration: 2s;
      #Logo { 0s { width: 10px } 1s { width: 50% } }
      #Gear { 0s { translate: 0px 0px; motion-out: 5px 0px } 1s { translate: 10px 10px; motion-in: -5px 0px } }
      #Dot1 { 0s { opacity: 0 } 1s { opacity: 1 } }
    }
  </timelines>
)X";
    const CompileResult plain = compileDoc(docWith("", timelines));
    requireCompiled(plain);
    SUBCASE("mixed units") {
        const CompileResult r =
            compileDoc(docWith("<expressions>intro #Logo width = value + 1</expressions>", timelines));
        requireCompiled(r);
        CHECK(findCode(r.diagnostics, "expr.units") != nullptr);
        CHECK(r.ir == plain.ir);
    }
    SUBCASE("motion path") {
        const CompileResult r = compileDoc(
            docWith("<expressions>intro #Gear translate.y = wiggle(1, 2)</expressions>", timelines));
        requireCompiled(r);
        CHECK(findCode(r.diagnostics, "expr.motion") != nullptr);
        CHECK(r.ir == plain.ir);
    }
    SUBCASE("duplicate: the first one wins") {
        const CompileResult r = compileDoc(
            docWith("<expressions>\n intro #Dot1 opacity = 0.25\n intro #Dot1 opacity = 0.75\n</expressions>",
                    timelines));
        requireCompiled(r);
        CHECK(findCode(r.diagnostics, "expr.duplicate") != nullptr);
        REQUIRE(r.expressions.size() == 2);
        CHECK(r.expressions[0].status == ExpressionOutcome::Status::Lowered);
        CHECK(r.expressions[1].status == ExpressionOutcome::Status::Skipped);
        LoadedDoc loaded(r.ir);
        CHECK(loaded.at("intro", 0.5f, "Dot1", prop::Opacity) == doctest::Approx(0.25f));
    }
    SUBCASE("no keys: the static value at 0 s, with a note") {
        const CompileResult r = compileDoc(
            docWith("<expressions>intro #Dot2 opacity = value * 0.5 + time * 0.1</expressions>", timelines));
        requireCompiled(r);
        CHECK(findCode(r.diagnostics, "expr.static") != nullptr);
        REQUIRE(outcomeFor(r, "Dot2", "opacity") != nullptr);
        CHECK(outcomeFor(r, "Dot2", "opacity")->status == ExpressionOutcome::Status::Lowered);
        LoadedDoc loaded(r.ir);
        CHECK(loaded.at("intro", 0.0f, "Dot2", prop::Opacity) == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(loaded.at("intro", 2.0f, "Dot2", prop::Opacity) == doctest::Approx(0.7f).epsilon(0.01));
    }
}

TEST_CASE("T-767 --strict: the expr.* notes do not fail a compile") {
    CompileOptions strict;
    strict.strict = true;
    const CompileResult r = compileDoc(
        docWith(
            "<expressions>\n intro #Ghost opacity = 1\n intro #Logo translate.y = wiggle(2,\n</expressions>"),
        strict);
    requireCompiled(r);
    CHECK(findCode(r.diagnostics, "expr.element") != nullptr);
    CHECK(findCode(r.diagnostics, "expr.syntax") != nullptr);
}

// ---- symbols ---------------------------------------------------------------------------------

TEST_CASE("T-767 a symbol's expressions apply per instance (index and seed per instance)") {
    const std::string text = R"X(<xt version="2">
  <symbol name="Card">
    <div id="Box"><div id="A"></div><div id="B"></div></div>
    <timelines>
      @timeline pop { duration: 2s; #B { 0s { opacity: 0 } 1s { opacity: 1 } } }
    </timelines>
    <expressions>
      pop #B opacity = index * 0.1 + time * 0.0
      pop #A translate.y = wiggle(3, 4)
    </expressions>
  </symbol>
  <body><use symbol="Card" id="C1"/><use symbol="Card" id="C2"/></body>
</xt>)X";
    const CompileResult r = compileFiles({SourceFile{"sym.xt", text}});
    requireCompiled(r);
    REQUIRE(r.expressions.size() == 4);
    CHECK(r.expressions[0].scopePath == "C1");
    CHECK(r.expressions[0].symbol == "Card");
    CHECK(r.expressions[1].scopePath == "C2");
    for (const ExpressionOutcome& o : r.expressions) {
        CHECK(o.status == ExpressionOutcome::Status::Lowered);
    }
    // #A has no keys in `pop`: a static-value note per instance.
    CHECK(countCode(r.diagnostics, "expr.static") == 2);
    LoadedDoc loaded(r.ir);
    REQUIRE(loaded.doc->seekTimeline("C1", "pop", 1.0f) == Status::kOk);
    REQUIRE(loaded.doc->seekTimeline("C2", "pop", 1.0f) == Status::kOk);
    loaded.ctx.update(0.5f);
    CHECK(loaded.doc->byId("C1/B").property(prop::Opacity).f == doctest::Approx(0.2f));  // index 2.
    CHECK(loaded.doc->byId("C2/B").property(prop::Opacity).f == doctest::Approx(0.2f));
    // Different instances, different seeds: the wiggles differ.
    bool differ = false;
    const auto& k1 = r.expressions[2].keys;
    const auto& k2 = r.expressions[3].keys;
    for (size_t i = 0; i < std::min(k1.size(), k2.size()); ++i) {
        differ = differ || std::fabs(k1[i].value - k2[i].value) > 1e-6;
    }
    CHECK((differ || k1.size() != k2.size()));
}

// ---- canonical writer ------------------------------------------------------------------------

TEST_CASE("T-767 the canonical writer: order, indentation and a stable round trip") {
    const std::string text = R"X(<xt version="2">
<body><div id="A"></div><div id="B"></div></body>
<timelines>
@timeline zed { #A { 0s { opacity: 0 } } }
@timeline intro { #B { 0s { opacity: 0 } } #A { 0s { opacity: 1 } } }
</timelines>
<expressions>
// zed first in the file
zed #A opacity = time
      intro #A opacity off   =   value*2
intro #B opacity = {   posterizeTime(12);
          var d = 0.1;
            valueAtTime(time - d)
   }
intro #Z opacity = 1
</expressions>
</xt>
)X";
    DiagnosticSink sink;
    CanonicalXt first;
    REQUIRE(readCanonicalXt(text, "c.xt", sink, first));
    REQUIRE(first.expressions.size() == 4);
    CHECK(first.expressions[1].off);
    CHECK(first.expressions[1].text == "value*2");
    CHECK(first.expressions[2].multiline);
    CHECK(first.expressions[2].text == "posterizeTime(12);\nvar d = 0.1;\n  valueAtTime(time - d)");
    const std::string written = writeCanonicalXt(first);
    const std::string expected =
        "  <expressions>\n"
        "    intro #B opacity = {\n"
        "      posterizeTime(12);\n"
        "      var d = 0.1;\n"
        "        valueAtTime(time - d)\n"
        "    }\n"
        "    intro #A opacity off = value*2\n"
        "    intro #Z opacity = 1\n"
        "    // zed first in the file\n"
        "    zed #A opacity = time\n"
        "  </expressions>\n"
        "</xt>\n";
    CAPTURE(written);
    REQUIRE(written.size() > expected.size());
    CHECK(written.substr(written.size() - expected.size()) == expected);
    CanonicalXt second;
    REQUIRE(readCanonicalXt(written, "c.xt", sink, second));
    CHECK(writeCanonicalXt(second) == written);
    // Symbol-level entries stay with their symbol.
    const std::string sym =
        "<xt version=\"2\"><symbol name=\"S\"><div id=\"X\"></div>"
        "<expressions>pop #X opacity = 1</expressions></symbol></xt>";
    CanonicalXt lib;
    REQUIRE(readCanonicalXt(sym, "s.xt", sink, lib));
    REQUIRE(lib.symbols.size() == 1);
    REQUIRE(lib.symbols[0].expressions.size() == 1);
    CHECK(lib.expressions.empty());
    CHECK(writeCanonicalXt(lib).find(
              "    <expressions>\n      pop #X opacity = 1\n    </expressions>\n  </symbol>") !=
          std::string::npos);
}

// ---- IR size ---------------------------------------------------------------------------------

TEST_CASE("T-767 IR size: 10 wiggle rows on a 15 s timeline") {
    std::string body = "<body>";
    std::string tracks;
    std::string entries;
    for (int i = 0; i < 10; ++i) {
        const std::string id = "W" + std::to_string(i);
        body += "<div id=\"" + id + "\"></div>";
        tracks += "#" + id + " { 0s { translate: 0px 0px } 15s { translate: 0px 0px } } ";
        entries += "  intro #" + id + " translate.y = wiggle(2, 6)\n";
    }
    body += "</body>";
    const std::string head = "<xt version=\"2\">" + body + "<timelines>@timeline intro { duration: 15s; " +
                             tracks + "}</timelines>";
    const CompileResult plain = compileDoc(head + "</xt>");
    const CompileResult wiggled = compileDoc(head + "<expressions>\n" + entries + "</expressions></xt>");
    requireCompiled(plain);
    requireCompiled(wiggled);
    size_t keys = 0;
    for (const ExpressionOutcome& o : wiggled.expressions) {
        CHECK(o.status == ExpressionOutcome::Status::Lowered);
        keys += o.keys.size();
    }
    MESSAGE("T-767 IR size: " << plain.ir.size() << " bytes -> " << wiggled.ir.size() << " bytes (+"
                              << (wiggled.ir.size() - plain.ir.size())
                              << ") for 10 wiggle(2, 6) rows over 15 s, " << keys << " lowered keys");
    CHECK(wiggled.ir.size() > plain.ir.size());
}

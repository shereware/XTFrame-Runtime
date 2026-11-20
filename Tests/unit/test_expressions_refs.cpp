#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt_v2_test_util.h"

/// T-770 (Docs/tasks/T-770.md, Docs/design/58-value-functions-and-drivers.md §58.2-§58.3): an
/// `<expressions>` entry reading another row of its timeline -- `layer("Name").<row>`,
/// `layer(<n>).<row>`, the AE aliases, `.valueAtTime(t)` -- lowered in dependency order, cycles, rows
/// of another timeline (run-time references for T-777), and effect-parameter and inline `<svg>` rows
/// as expression targets.

using namespace xt;
using namespace xt::compile;
using xt::test::v2::compileFiles;
using xt::test::v2::findCode;
using xt::test::v2::requireCompiled;

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

    void seek(const char* name, float t) {
        REQUIRE(doc->seekTimeline("", name, t) == Status::kOk);
        clock += 0.01f;
        ctx.update(clock);
    }

    /// `prop` of `id` with timeline `name` seeked to `t`.
    float at(const char* name, float t, const char* id, PropId prop) {
        seek(name, t);
        return doc->byId(id).property(prop).f;
    }
};

CompileResult compileDoc(const std::string& text) { return compileFiles({SourceFile{"r.xt", text}}); }

std::string doc(const std::string& body, const std::string& timelines, const std::string& expressions,
                const std::string& style = "") {
    return "<xt version=\"2\">" + (style.empty() ? std::string() : "<style>" + style + "</style>") +
           "<body>" + body + "</body><timelines>" + timelines + "</timelines>" +
           (expressions.empty() ? std::string() : "<expressions>\n" + expressions + "</expressions>") +
           "</xt>\n";
}

size_t countCode(const std::vector<Diagnostic>& diags, const std::string& code) {
    return static_cast<size_t>(
        std::count_if(diags.begin(), diags.end(), [&](const Diagnostic& d) { return d.code == code; }));
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

ExpressionOutcome::Status statusOf(const CompileResult& r, const std::string& element,
                                   const std::string& row) {
    const ExpressionOutcome* o = outcomeFor(r, element, row);
    REQUIRE(o != nullptr);
    return o->status;
}

/// The note with `code` whose message mentions `needle`.
const Diagnostic* noteWith(const CompileResult& r, const std::string& code, const std::string& needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.code == code && d.message.find(needle) != std::string::npos) {
            return &d;
        }
    }
    return nullptr;
}

/// A 0 -> 100 px ramp over [0, 2] s, held after.
double ramp(double t) { return std::clamp(t, 0.0, 2.0) * 50.0; }

constexpr const char* kLeaderBody = R"(<div id="Leader"></div><div id="Follower"></div>)";
constexpr const char* kLeaderTimeline = R"(
  @timeline intro {
    duration: 3s;
    #Leader { 0s { translate: 0px 0px } 2s { translate: 0px 100px } }
    #Follower { 0s { translate: 0px 0px } }
  }
)";

}  // namespace

// ---- references by name ----------------------------------------------------------------------

TEST_CASE("T-770 delayed follow: layer(\"Leader\").y.valueAtTime(time - 0.2)") {
    const CompileResult r =
        compileDoc(doc(kLeaderBody, kLeaderTimeline,
                       "intro #Follower translate.y = layer(\"Leader\").y.valueAtTime(time - 0.2)\n"));
    requireCompiled(r);
    REQUIRE(statusOf(r, "Follower", "translate.y") == ExpressionOutcome::Status::Lowered);
    CHECK(outcomeFor(r, "Follower", "translate.y")->diagnostics.empty());
    LoadedDoc loaded(r.ir);
    for (int k = 0; k <= 180; k += 9) {
        const double t = k / 60.0;
        CAPTURE(t);
        CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), "Follower", prop::TranslateY) -
                        ramp(t - 0.2)) <= 0.25 + 1e-3);
    }
    // The leader itself is untouched.
    CHECK(loaded.at("intro", 1.0f, "Leader", prop::TranslateY) == doctest::Approx(50.0f));
}

TEST_CASE("T-770 a chain of 5 siblings, each following the previous one, written last-first") {
    const std::string body = R"(<div id="Row"><div id="D1"></div><div id="D2"></div><div id="D3"></div>)"
                             R"(<div id="D4"></div><div id="D5"></div></div>)";
    const std::string timelines = R"(
  @timeline intro {
    duration: 3s;
    #D1 { 0s { translate: 0px 0px } 2s { translate: 0px 100px } }
    #D2 { 0s { translate: 0px 0px } } #D3 { 0s { translate: 0px 0px } }
    #D4 { 0s { translate: 0px 0px } } #D5 { 0s { translate: 0px 0px } }
  }
)";
    std::string entries;
    for (int i = 5; i >= 2; --i) {  // Reverse order: each entry needs the one written after it.
        entries += "intro #D" + std::to_string(i) +
                   " translate.y = layer(index - 1).translate.y.valueAtTime(time - 0.1)\n";
    }
    // The guarded AE idiom on every sibling (the first one keeps its value).
    for (int i = 1; i <= 5; ++i) {
        entries += "intro #D" + std::to_string(i) +
                   " translate.x = index > 1 ? layer(index - 1).x.valueAtTime(time - 0.1) : value\n";
    }
    const CompileResult r = compileDoc(doc(body, timelines, entries));
    requireCompiled(r);
    for (const ExpressionOutcome& o : r.expressions) {
        CAPTURE(o.element);
        CAPTURE(o.property);
        CHECK(o.status == ExpressionOutcome::Status::Lowered);
        CHECK(o.diagnostics.empty());
    }
    // The outcomes keep the file order.
    REQUIRE(r.expressions.size() == 9);
    CHECK(r.expressions[0].element == "D5");
    CHECK(r.expressions[3].element == "D2");
    LoadedDoc loaded(r.ir);
    const char* ids[] = {"D1", "D2", "D3", "D4", "D5"};
    for (int k = 0; k <= 180; k += 10) {
        const double t = k / 60.0;
        CAPTURE(t);
        for (int i = 0; i < 5; ++i) {
            CAPTURE(i);
            // Each link adds at most one tolerance (0.25 px) of error.
            CHECK(std::fabs(loaded.at("intro", static_cast<float>(t), ids[i], prop::TranslateY) -
                            ramp(t - 0.1 * i)) <= 0.25 * i + 1e-3);
        }
    }
}

TEST_CASE("T-770 the referenced row has an expression too: its lowered keys are read") {
    const std::string body = R"(<div id="A"></div><div id="B"></div>)";
    const std::string timelines =
        "@timeline intro { duration: 2s; #A { 0s { rotate: 0deg } } #B { 0s { rotate: 0deg } } }";
    const CompileResult r = compileDoc(doc(body, timelines,
                                           "intro #B rotate = layer(\"A\").rotation * 2\n"
                                           "intro #A rotate = value + time * 90\n"));
    requireCompiled(r);
    CHECK(statusOf(r, "A", "rotate") == ExpressionOutcome::Status::Lowered);
    CHECK(statusOf(r, "B", "rotate") == ExpressionOutcome::Status::Lowered);
    LoadedDoc loaded(r.ir);
    for (float t : {0.0f, 0.5f, 1.25f, 2.0f}) {
        CAPTURE(t);
        CHECK(loaded.at("intro", t, "A", prop::Rotation) == doctest::Approx(90.0f * t).epsilon(0.002));
        CHECK(loaded.at("intro", t, "B", prop::Rotation) == doctest::Approx(180.0f * t).epsilon(0.002));
    }
}

TEST_CASE("T-770 a cycle: expr.cycle on each entry of it; an entry outside reads the rows' own keys") {
    const std::string body = R"(<div id="A"></div><div id="B"></div><div id="C"></div><div id="S"></div>)";
    const std::string timelines = R"(
  @timeline intro {
    duration: 1s;
    #A { 0s { opacity: 0 } 1s { opacity: 1 } }
    #B { 0s { opacity: 1 } }
    #C { 0s { opacity: 0 } }
    #S { 0s { opacity: 0.5 } }
  }
)";
    const CompileResult plain = compileDoc(doc(body, timelines, ""));
    const CompileResult r = compileDoc(doc(body, timelines,
                                           "intro #C opacity = layer(\"A\").opacity * 0.5\n"
                                           "intro #A opacity = layer(\"B\").opacity\n"
                                           "intro #B opacity = 1 - layer(\"A\").opacity\n"
                                           "intro #S opacity = layer(\"S\").opacity + 0.1\n"));
    requireCompiled(r);
    requireCompiled(plain);
    CHECK(countCode(r.diagnostics, "expr.cycle") == 3);
    CHECK(statusOf(r, "A", "opacity") == ExpressionOutcome::Status::Skipped);
    CHECK(statusOf(r, "B", "opacity") == ExpressionOutcome::Status::Skipped);
    CHECK(statusOf(r, "S", "opacity") == ExpressionOutcome::Status::Skipped);
    REQUIRE(statusOf(r, "C", "opacity") == ExpressionOutcome::Status::Lowered);
    const Diagnostic* cycle = noteWith(r, "expr.cycle", "#A opacity");
    REQUIRE(cycle != nullptr);
    CHECK(cycle->message.find("#B opacity") != std::string::npos);
    CHECK(cycle->level == Diagnostic::Level::Warning);
    CHECK(noteWith(r, "expr.cycle", "#S opacity -> #S opacity") != nullptr);
    LoadedDoc loaded(r.ir);
    CHECK(loaded.at("intro", 0.5f, "A", prop::Opacity) == doctest::Approx(0.5f));  // Its own keys.
    CHECK(loaded.at("intro", 0.5f, "B", prop::Opacity) == doctest::Approx(1.0f));
    CHECK(loaded.at("intro", 0.5f, "C", prop::Opacity) == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(loaded.at("intro", 0.5f, "S", prop::Opacity) == doctest::Approx(0.5f));
}

TEST_CASE("T-770 a row of another timeline is a run-time reference (T-777: compiled into TEXP)") {
    const std::string body = R"(<div id="A"></div><div id="X"></div>)";
    const std::string timelines = R"(
  @timeline intro { duration: 1s; #A { 0s { translate: 0px 0px } 1s { translate: 0px 10px } } }
  @timeline other { duration: 1s; #X { 0s { translate: 0px 0px } 1s { translate: 0px 50px } } }
)";
    const CompileResult plain = compileDoc(doc(body, timelines, ""));
    requireCompiled(plain);
    const std::string expressions =
        "intro #A translate.y = layer(\"X\").y + 1\n"
        "intro #A opacity = layer(\"A\").scale.x\n";  // Not keyed in this timeline.
    const std::string text = doc(body, timelines, expressions);
    const CompileResult r = compileDoc(text);
    requireCompiled(r);
    CHECK(r.ir != plain.ir);  // T-777: both are run-time programs.
    CHECK_FALSE(xt::test::v2::chunkPayload(r.ir, "TEXP").empty());
    const ExpressionOutcome* o = outcomeFor(r, "A", "translate.y");
    REQUIRE(o != nullptr);
    CHECK(o->status == ExpressionOutcome::Status::NeedsRuntime);
    CHECK(o->runtimeInputs);
    REQUIRE(o->runtimeRefs.size() == 1);
    CHECK(o->runtimeRefs[0].key == "\"X\"");
    CHECK(o->runtimeRefs[0].element == "X");
    CHECK(o->runtimeRefs[0].row == "translate.y");
    CHECK(o->diagnostics.empty());  // T-777: no longer an expr.ref note.
    const ExpressionOutcome* s = outcomeFor(r, "A", "opacity");
    REQUIRE(s != nullptr);
    CHECK(s->status == ExpressionOutcome::Status::NeedsRuntime);
    REQUIRE(s->runtimeRefs.size() == 1);
    CHECK(s->runtimeRefs[0].row == "scale.x");
    REQUIRE(s->diagnostics.size() == 1);
    CHECK(s->diagnostics[0].code == "expr.static");  // The row had no keys: one with its static value.
}

TEST_CASE("T-770 failed references: unknown element, unknown row, the sibling before the first") {
    const std::string body = R"(<div id="P"><div id="A"></div><div id="B"></div></div>)";
    const std::string timelines =
        "@timeline intro { duration: 1s; #A { 0s { opacity: 0 } 1s { opacity: 1 } } #B { 0s { opacity: 1 } } "
        "}";
    struct Case {
        const char* entry;
        const char* needle;
    };
    const Case cases[] = {
        {"intro #B opacity = layer(\"Ghost\").opacity", "no element with id \"Ghost\""},
        {"intro #B opacity = layer(\"A\").frobnicate", "unknown row \"frobnicate\""},
        {"intro #B opacity = layer(\"A\").x.y", "names a component twice"},
        {"intro #B opacity = layer(\"A\").scale", "needs a component"},
        {"intro #A opacity = layer(index - 1).opacity", "no sibling before the first"},
        {"intro #A opacity = layer(3).opacity", "2 siblings"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.entry);
        const CompileResult r = compileDoc(doc(body, timelines, std::string(c.entry) + "\n"));
        requireCompiled(r);
        const Diagnostic* d = noteWith(r, "expr.ref", c.needle);
        REQUIRE(d != nullptr);
        CHECK(d->level == Diagnostic::Level::Warning);
        REQUIRE(r.expressions.size() == 1);
        CHECK(r.expressions[0].status == ExpressionOutcome::Status::Skipped);
    }
}

// ---- aliases and indices -----------------------------------------------------------------------

TEST_CASE("T-770 the AE aliases name the timeline rows") {
    const std::string body = R"(<div id="Src"></div><div id="Dst"></div>)";
    const std::string timelines = R"(
  @timeline intro {
    duration: 1s;
    #Src { 0s { translate: 3px 4px; rotate: 30deg; scale: 2 3; opacity: 0.25 } }
    #Dst { 0s { opacity: 0 } }
  }
)";
    struct Case {
        const char* ref;
        double expected;
    };
    const Case cases[] = {
        {"position.x", 3}, {"position.y", 4}, {"translate.y", 4}, {"x", 3},       {"y", 4},
        {"xPosition", 3},  {"yPosition", 4},  {"rotation", 30},   {"rotate", 30}, {"zRotation", 30},
        {"scale.x", 2},    {"scale.y", 3},    {"opacity", 0.25},
    };
    for (const Case& c : cases) {
        CAPTURE(c.ref);
        const CompileResult r = compileDoc(
            doc(body, timelines, std::string("intro #Dst opacity = layer(\"Src\").") + c.ref + " / 100\n"));
        requireCompiled(r);
        const ExpressionOutcome* o = outcomeFor(r, "Dst", "opacity");
        REQUIRE(o != nullptr);
        REQUIRE(o->status == ExpressionOutcome::Status::Lowered);
        REQUIRE_FALSE(o->keys.empty());
        CHECK(o->keys.front().value == doctest::Approx(c.expected / 100));
    }
}

TEST_CASE("T-770 references by sibling index, and inside a symbol instance") {
    const std::string body = R"(<div id="P"><div id="A"></div><div id="B"></div><div id="C"></div></div>)";
    const std::string timelines = R"(
  @timeline intro {
    duration: 1s;
    #A { 0s { opacity: 0.2 } } #B { 0s { opacity: 0.4 } } #C { 0s { opacity: 0 } }
  }
)";
    const CompileResult r =
        compileDoc(doc(body, timelines, "intro #C opacity = layer(1).opacity + layer(2).opacity\n"));
    requireCompiled(r);
    LoadedDoc loaded(r.ir);
    CHECK(loaded.at("intro", 0.5f, "C", prop::Opacity) == doctest::Approx(0.6f));

    const std::string sym = R"X(<xt version="2">
  <symbol name="Pair">
    <div id="Box"><div id="L"></div><div id="F"></div></div>
    <timelines>
      @timeline pop { duration: 1s; #L { 0s { opacity: 0 } 1s { opacity: 1 } } #F { 0s { opacity: 0 } } }
    </timelines>
    <expressions>
      pop #F opacity = layer("L").opacity * 0.5
    </expressions>
  </symbol>
  <body><use symbol="Pair" id="P1"/><use symbol="Pair" id="P2"/></body>
</xt>)X";
    const CompileResult s = compileFiles({SourceFile{"sym.xt", sym}});
    requireCompiled(s);
    REQUIRE(s.expressions.size() == 2);
    for (const ExpressionOutcome& o : s.expressions) {
        CHECK(o.status == ExpressionOutcome::Status::Lowered);
    }
    LoadedDoc inst(s.ir);
    REQUIRE(inst.doc->seekTimeline("P2", "pop", 1.0f) == Status::kOk);
    inst.ctx.update(0.5f);
    CHECK(inst.doc->byId("P2/F").property(prop::Opacity).f == doctest::Approx(0.5f));
}

// ---- effect-parameter and inline <svg> rows ---------------------------------------------------

TEST_CASE("T-770 an effect-parameter row as an expression target") {
    const std::string style = "#Card { effect: shadow(0px 2px 4px #000000) glow(8px #ffffff 0.2); }";
    const std::string body = R"(<div id="Card"></div>)";
    const std::string timelines =
        "@timeline pulse { duration: 2s; #Card { 0s { effect.glow.intensity: 0 } 1s { effect.glow.intensity: "
        "1 } "
        "0s { effect.0.offsetX: 0 } 0s { effect.glow.color: #ffffff } } }";
    const CompileResult plain = compileDoc(doc(body, timelines, "", style));
    requireCompiled(plain);
    const CompileResult r = compileDoc(
        doc(body, timelines, "pulse #Card effect.glow.intensity = Math.min(value * 0.5 + time * 0.25, 1)\n",
            style));
    requireCompiled(r);
    REQUIRE(statusOf(r, "Card", "effect.glow.intensity") == ExpressionOutcome::Status::Lowered);
    LoadedDoc loaded(r.ir);
    for (float t : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}) {
        CAPTURE(t);
        loaded.seek("pulse", t);
        const float expected = std::min(std::min(t, 1.0f) * 0.5f + t * 0.25f, 1.0f);
        CHECK(loaded.doc->byId("Card").effect(1).params[1] == doctest::Approx(expected).epsilon(0.005));
    }
    // By stack index.
    const CompileResult byIndex =
        compileDoc(doc(body, timelines, "pulse #Card effect.0.offsetX = time * 4\n", style));
    requireCompiled(byIndex);
    CHECK(statusOf(byIndex, "Card", "effect.0.offsetX") == ExpressionOutcome::Status::Lowered);
    LoadedDoc shifted(byIndex.ir);
    shifted.seek("pulse", 1.5f);
    CHECK(shifted.doc->byId("Card").effect(0).params[0] == doctest::Approx(6.0f).epsilon(0.005));
    // What cannot be done: a color, an unkeyed parameter, a malformed address, a shape with effects.
    struct Case {
        const char* entry;
        const char* needle;
    };
    const Case cases[] = {
        {"pulse #Card effect.glow.color = 1", "not one number"},
        {"pulse #Card effect.glow.radius = value + 1", "no keys in this timeline"},
        {"pulse #Card effect.1.intensity = value + 1", "different spellings"},
        {"pulse #Card effect.glow = 1", "effect.<stack index or name>.<param>"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.entry);
        const CompileResult bad = compileDoc(doc(body, timelines, std::string(c.entry) + "\n", style));
        requireCompiled(bad);
        CHECK(noteWith(bad, "expr.property", c.needle) != nullptr);
        CHECK(bad.ir == plain.ir);
    }
}

TEST_CASE("T-770 rows of shapes inside an inline <svg> as expression targets") {
    const std::string body = R"(
      <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
        <g id="g"><path id="p" d="M0 0 L10 0 L10 10 Z" fill="#ff0000"/></g>
        <path id="line" d="M0 16 L32 16" stroke="#000000"/>
      </svg>)";
    const std::string timelines = R"(
  @timeline main {
    duration: 1s;
    #Icon/line { 0s { path-trim-end: 0 } 1s { path-trim-end: 1 } }
    #Icon/g { 0s { rotate: 0deg } 1s { rotate: 90deg } }
  }
)";
    const CompileResult plain = compileDoc(doc(body, timelines, ""));
    requireCompiled(plain);
    const CompileResult r = compileDoc(doc(body, timelines,
                                           "main #Icon/line path-trim-end = 1 - value\n"
                                           "main #Icon/g rotate = layer(\"Icon/line\").pathTrimEnd * 180\n"));
    requireCompiled(r);
    REQUIRE(statusOf(r, "Icon/line", "path-trim-end") == ExpressionOutcome::Status::Lowered);
    REQUIRE(statusOf(r, "Icon/g", "rotate") == ExpressionOutcome::Status::Lowered);
    LoadedDoc loaded(r.ir);
    std::vector<NodeId> paths;
    std::vector<NodeId> groups;
    loaded.doc->arena().traversePreorder(loaded.doc->byId("Icon").node, [&](NodeId n) {
        if (loaded.doc->arena().kind(n) == NodeKind::Path) {
            paths.push_back(n);
        } else if (loaded.doc->arena().kind(n) == NodeKind::Group) {
            groups.push_back(n);
        }
        return true;
    });
    REQUIRE(paths.size() == 2);
    REQUIRE(groups.size() >= 2);
    loaded.seek("main", 0.25f);
    CHECK(loaded.doc->arena().path(paths[1]).trimEnd == doctest::Approx(0.75f).epsilon(0.01));
    CHECK(loaded.doc->arena().common(groups[1]).rotation == doctest::Approx(135.0f).epsilon(0.01));
    // What cannot be done: an unkeyed row, effects, the sibling index.
    struct Case {
        const char* entry;
        const char* code;
        const char* needle;
    };
    const Case cases[] = {
        {"main #Icon/p opacity = 0.5", "expr.property", "no static value"},
        {"main #Icon/line effect.glow.intensity = 1", "expr.property", "no effects"},
        {"main #Icon/line path-trim-end = layer(index).pathTrimEnd", "expr.ref", "no sibling index"},
        {"main #Icon path-trim-end = 1", "expr.property", "inline <svg>"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.entry);
        const CompileResult bad = compileDoc(doc(body, timelines, std::string(c.entry) + "\n"));
        requireCompiled(bad);
        CHECK(noteWith(bad, c.code, c.needle) != nullptr);
        CHECK(bad.ir == plain.ir);
    }
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "expr/expr.h"
#include "expr/expr_runtime.h"
#include "expr/expr_split.h"
#include "expressions_section.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/document.h"
#include "xt/expr_noise.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt_v2_test_util.h"

/// T-777 (Docs/tasks/T-777.md, Docs/design/58-value-functions-and-drivers.md §58.5, §58.5a): timeline
/// expressions with run-time inputs compiled into the IR's `TEXP` chunk -- hidden curves for the
/// time-only parts, ops for the rest -- checked end to end (compile, load, seek, read the property)
/// against the compile-time evaluator fed the same inputs (ExprRuntimeInputs); the same time-only
/// expression down both paths; the rejections of 58 §58.5a; the TEXP round trip through
/// IrWriter::fromView; the `noise()` ruling.

using namespace xt;
using namespace xt::compile;
namespace xe = xt::compile::expr;
using xt::test::v2::chunkPayload;
using xt::test::v2::compileFiles;
using xt::test::v2::requireCompiled;

namespace {

constexpr const char* kHead = R"(<xt version="2">
  <ports>
    hp: number in
    k: number in
    shake: number in
    f: number in
    d: number in
    title: string in
  </ports>
  <body>
    <div id="Box"></div>
    <div id="A"></div>
    <div id="Label"></div>
    <div id="Other"></div>
  </body>
  <layout>
    #Box { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
    #A { position: absolute; left: 50px; top: 0px; width: 10px; height: 10px; }
    #Label { position: absolute; left: 10px; top: 50px; width: 70px; height: 20px; }
    #Other { position: absolute; left: 100px; top: 0px; width: 10px; height: 10px; }
  </layout>
  <timelines>
    @timeline intro {
      duration: 2s;
      #Box { 0s { translate: 0px 0px; opacity: 0.2 } 2s { translate: 0px 40px; opacity: 0.8 } }
      #A { 0s { translate: 0px 0px } 2s { translate: 0px 100px } }
    }
    @timeline other {
      duration: 2s;
      #Other { 0s { translate: 0px 10px } 2s { translate: 0px 30px } }
    }
  </timelines>
)";

/// The document with `expressions` (entry lines) in its `<expressions>` section.
std::string docText(const std::string& expressions) {
    return std::string(kHead) +
           (expressions.empty() ? "" : "  <expressions>\n" + expressions + "  </expressions>\n") + "</xt>\n";
}

CompileResult compileText(const std::string& text, bool allAtRunTime = false) {
    CompileOptions options;
    options.expressionsAtRunTime = allAtRunTime;
    return compileFiles({SourceFile{"rt.xt", text}}, options);
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

/// The TEXP chunk of `ir` (REQUIREs it opens).
struct Texp {
    std::vector<uint8_t> ir;
    std::unique_ptr<IrView> view;
    explicit Texp(const std::vector<uint8_t>& bytes) : ir(bytes) {
        Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
        REQUIRE(opened.ok());
        view = std::make_unique<IrView>(std::move(opened.value()));
    }
    const TimelineExprChunk& chunk() const { return view->timelineExprs(); }
    std::vector<ExprOp> ops(size_t expr) const {
        const IrTimelineExpr& e = chunk().exprs()[expr];
        std::vector<ExprOp> out;
        for (uint32_t i = 0; i < e.opCount; ++i) {
            out.push_back(static_cast<ExprOp>(chunk().ops()[e.opOffset + i].op));
        }
        return out;
    }
};

/// The compiled document, loaded.
struct Loaded {
    std::vector<uint8_t> ir;
    std::unique_ptr<Document> doc;
    PerfTracker perf;
    float clock = 0;

    explicit Loaded(const std::vector<uint8_t>& bytes) : ir(bytes) {
        REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
        perf.setEnabled(true);
        doc->setPerfTracker(&perf);
        doc->setViewport(Rect{0, 0, 400, 300});
        step();
    }
    /// One update `dt` seconds later; its FrameStats::exprEvaluated.
    uint32_t step(float dt = 0.01f) {
        clock += dt;
        perf.beginFrame(clock);
        doc->update(clock);
        return perf.current().exprEvaluated;
    }
    void port(const char* name, float v) {
        const PortId id = doc->lookupPort(name);
        REQUIRE(id != 0);
        REQUIRE(doc->setPort(id, Value::ofFloat(v)) == Status::kOk);
    }
    float prop(const char* id, PropId p) { return doc->byId(id).property(p).f; }
    /// `id`'s translate.y with `intro` seeked to `t` (two updates: a layout read lags one).
    float at(float t, const char* id = "Box") {
        REQUIRE(doc->seekTimeline("", "intro", t) == Status::kOk);
        step();
        step();
        return prop(id, prop::TranslateY);
    }
};

const std::vector<xe::ExprKey>& boxKeys() {
    static const std::vector<xe::ExprKey> keys{{0, 0, Easing::linear()}, {2, 40, Easing::linear()}};
    return keys;
}
const std::vector<xe::ExprKey>& aKeys() {
    static const std::vector<xe::ExprKey> keys{{0, 0, Easing::linear()}, {2, 100, Easing::linear()}};
    return keys;
}

/// The compile-time side of `#Box translate.y`: its row, seed and index, `layer("A")` of intro,
/// `layer("Other")` another timeline's row, and the stand-ins for the run-time inputs.
struct Reference {
    std::map<std::string, double> ports;
    double labelWidth = 70;
    double otherY = 0;
    xe::ExprRuntimeInputs inputs;
    xe::ExprContext ctx;

    Reference() {
        inputs.port = [this](std::string_view name, double) {
            const auto it = ports.find(std::string(name));
            return it != ports.end() ? it->second : 0.0;
        };
        inputs.layout = [this](const xe::ExprLayerKey& key, int which) {
            return key.name == "Label" && which == 2 ? labelWidth : 0.0;
        };
        inputs.layerProp = [this](const xe::ExprLayerKey&, std::string_view, double) { return otherY; };
        ctx.row = xe::ExprRow{boxKeys(), {}};
        ctx.duration = 2;
        ctx.index = 1;
        ctx.seed = expressionSeed("intro", "Box", "translate", "y");
        ctx.component = 1;
        ctx.tolerance = 0.25;
        ctx.otherRows = [](const xe::ExprLayerKey& key, std::string_view prop) {
            xe::ExprRowRef ref;
            if (key.name == "A" && prop == "y") {
                ref.kind = xe::ExprRefKind::Row;
                ref.row = xe::ExprRow{aKeys(), {}};
            } else if (key.name == "Other") {
                ref.kind = xe::ExprRefKind::Runtime;
            }
            return ref;
        };
        ctx.runtimeInputs = &inputs;
    }

    /// evaluateExpression() of `entryText` (an entry's text: a `{ ... }` block is unwrapped) at `t`;
    /// `split`: splitRuntime()'s evaluator instead.
    double value(const std::string& entryText, double t, bool split = false) {
        std::string text = entryText;
        if (text.size() > 2 && text.front() == '{' && text.back() == '}') {
            text = text.substr(1, text.size() - 2);
        }
        const xe::ParseResult parsed = xe::parseExpression(text);
        REQUIRE(parsed.ok());
        if (split) {
            const xe::SplitResult s = xe::splitRuntime(parsed.program, ctx);
            REQUIRE(s.status == xe::ExprStatus::Ok);
            double out = 0;
            std::vector<Diagnostic> diags;
            REQUIRE(xe::evalSplitComponent(parsed.program, s, s.isArray ? 1 : 0, ctx, t, out, diags) ==
                    xe::ExprStatus::Ok);
            return out;
        }
        const xe::EvalResult r = xe::evaluateExpression(parsed.program, ctx, t);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        double out = 0;
        std::vector<Diagnostic> diags;
        REQUIRE(xe::componentOf(r.value, 1, out, diags));
        return out;
    }
};

/// Sample times (multiples of 1/60 s, where a hidden curve is within half the tolerance of its
/// expression), none on a posterizeTime(4 / 5) step.
constexpr int kSampleFrames[] = {0, 7, 19, 33, 50, 77, 101, 115};

/// Compiles `#Box translate.y = text`, runs it for every port set and sample time and compares it
/// with the reference (within the curve tolerance scaled by the largest port value).
void checkAgainstReference(const std::string& text,
                           const std::vector<std::map<std::string, double>>& portSets,
                           bool splitReference = false) {
    CAPTURE(text);
    const CompileResult r = compileText(docText("    intro #Box translate.y = " + text + "\n"));
    requireCompiled(r);
    const ExpressionOutcome* o = outcomeFor(r, "Box", "translate.y");
    REQUIRE(o != nullptr);
    for (const Diagnostic& d : o->diagnostics) {
        CAPTURE(d.message);
    }
    REQUIRE(o->status == ExpressionOutcome::Status::NeedsRuntime);
    CHECK(o->runtimeInputs);
    CHECK_FALSE(chunkPayload(r.ir, "TEXP").empty());
    Loaded loaded(r.ir);
    Reference ref;
    for (const auto& ports : portSets) {
        double scale = 1;
        for (const auto& [name, value] : ports) {
            loaded.port(name.c_str(), static_cast<float>(value));
            scale = std::max(scale, std::fabs(value));
        }
        ref.ports = ports;
        for (int k : kSampleFrames) {
            const double t = k / 60.0;
            CAPTURE(t);
            CAPTURE(ports.size());
            const double got = loaded.at(static_cast<float>(t));
            const double want = ref.value(text, t, splitReference);
            CHECK(std::fabs(got - want) <= 0.15 * scale + 1e-3 * (1 + std::fabs(want)));
        }
    }
}

}  // namespace

// ---- each example against the compile-time evaluator --------------------------------------------

TEST_CASE("T-777 wiggle(2, 6) * port(\"shake\"): one hidden curve times the port") {
    const std::string text = "wiggle(2, 6) * port(\"shake\")";
    const CompileResult r = compileText(docText("    intro #Box translate.y = " + text + "\n"));
    requireCompiled(r);
    const Texp texp(r.ir);
    REQUIRE(texp.chunk().exprs().size() == 1);
    const IrTimelineExpr& e = texp.chunk().exprs()[0];
    CHECK(e.curveCount == 1);
    CHECK(texp.ops(0) == std::vector<ExprOp>{ExprOp::PushCurve, ExprOp::PushPort, ExprOp::Mul});
    CHECK(e.inputMask == kExprInputPort);
    CHECK(e.maxStack == 2);
    CHECK(e.seed == expressionSeed("intro", "Box", "translate", "y"));
    CHECK(e.index == 1.0f);
    checkAgainstReference(text, {{{"shake", 0}}, {{"shake", 1}}, {{"shake", -1.5}}});
}

TEST_CASE("T-777 the examples of the sheet equal the compile-time evaluator with the same inputs") {
    const std::vector<std::map<std::string, double>> hp = {{{"hp", 0.1}}, {{"hp", 0.5}}, {{"hp", 0.9}}};
    const std::vector<std::map<std::string, double>> k = {{{"k", 0}}, {{"k", 0.5}}, {{"k", 2}}};
    checkAgainstReference("value * port(\"k\")", k);
    checkAgainstReference("linear(port(\"hp\"), 0, 1, 20, 200)", hp);
    checkAgainstReference("port(\"hp\") < 0.2 ? wiggle(8, 4) : value", hp);
    checkAgainstReference("layer(\"Label\").width + 24", {{}});
    checkAgainstReference("valueAtTime(time - port(\"d\"))", {{{"d", 0}}, {{"d", 0.3}}, {{"d", 1.5}}});
    checkAgainstReference("layer(\"A\").y.valueAtTime(time - port(\"d\"))", {{{"d", 0.25}}, {{"d", 1}}});
    checkAgainstReference("port(\"hp\").valueAtTime(time - 0.5)", hp);
    checkAgainstReference(
        "{\n      var s = 0;\n      for (var i = 1; i <= 3; i++) { s += port(\"k\") * i; }\n"
        "      value + s\n    }",
        k);
    checkAgainstReference("{\n      function f(x) { return x * 2 + port(\"k\"); }\n      f(value)\n    }", k);
    checkAgainstReference("[wiggle(2, 6), 0][0] * port(\"k\")", k);
    checkAgainstReference("ease(port(\"hp\"), 0, 1, value, 100) + noise(time * 3) * 5", hp);
    checkAgainstReference("Math.max(port(\"k\"), 1) * Math.sin(time * 4) * 10 + value % 7", k);
    // posterizeTime(): the curve read at the quantized time (CurveAt); seedRandom(): RandomSeeded.
    checkAgainstReference("posterizeTime(4); wiggle(1, 10) + port(\"k\")", k);
    checkAgainstReference("seedRandom(7, true); random(0, 5) + port(\"k\")", k);
    checkAgainstReference("seedRandom(7, true); random(-3, port(\"k\")) + value", k);
    checkAgainstReference("seedRandom(3); random(port(\"k\"), 5)", k);
    checkAgainstReference("random(0, port(\"hp\")) + value", hp);
}

TEST_CASE("T-777 wiggle(port(\"f\"), 6): the run-time frequency, not periodic over the duration") {
    // The run-time evaluator repeats a wiggle over the duration only for a constant frequency (58
    // §58.5: the phase jumps when the frequency changes, as in AE); splitRuntime's evaluator agrees.
    const std::string text = "wiggle(port(\"f\"), 6)";
    const CompileResult r = compileText(docText("    intro #Box translate.y = " + text + "\n"));
    requireCompiled(r);
    const Texp texp(r.ir);
    REQUIRE(texp.chunk().exprs().size() == 1);
    CHECK(texp.ops(0) == std::vector<ExprOp>{ExprOp::PushPort, ExprOp::PushConst, ExprOp::Wiggle});
    CHECK(texp.chunk().exprs()[0].curveCount == 0);
    checkAgainstReference(text, {{{"f", 1}}, {{"f", 3.7}}}, /*splitReference=*/true);
}

TEST_CASE("T-777 the op forms: posterizeTime, seedRandom, delayed ports, rows, layout, per-play random") {
    struct Case {
        const char* text;
        std::vector<ExprOp> ops;
    };
    const Case cases[] = {
        {"posterizeTime(4); wiggle(1, 10) + port(\"k\")",
         {ExprOp::PushTime, ExprOp::PushConst, ExprOp::Mul, ExprOp::PushConst, ExprOp::Add, ExprOp::Floor,
          ExprOp::PushConst, ExprOp::Div, ExprOp::CurveAt, ExprOp::PushPort, ExprOp::Add}},
        {"seedRandom(7, true); random(0, port(\"k\"))",
         {ExprOp::PushConst, ExprOp::PushConst, ExprOp::PushPort, ExprOp::RandomSeeded}},
        // A timeless draw between constants is a constant at compile time.
        {"seedRandom(7, true); random(0, 5) + port(\"k\")",
         {ExprOp::PushConst, ExprOp::PushPort, ExprOp::Add}},
        {"port(\"hp\").valueAtTime(time - 0.5)", {ExprOp::PushConst, ExprOp::PushPortDelayed}},
        {"valueAtTime(time - port(\"d\"))",
         {ExprOp::PushTime, ExprOp::PushPort, ExprOp::Sub, ExprOp::ValueAt}},
        {"layer(\"A\").y.valueAtTime(time - port(\"d\"))",
         {ExprOp::PushTime, ExprOp::PushPort, ExprOp::Sub, ExprOp::RefValueAt}},
        {"layer(\"Label\").width + 24", {ExprOp::PushLayout, ExprOp::PushConst, ExprOp::Add}},
        {"layer(\"Other\").y", {ExprOp::PushProp}},
        {"randomPerPlay(0, 10)",
         {ExprOp::PushConst, ExprOp::PushConst, ExprOp::PushPlayRandom, ExprOp::Lerp}},
    };
    for (const Case& c : cases) {
        const std::string text = c.text;
        CAPTURE(text);
        const CompileResult r = compileText(docText("    intro #Box translate.y = " + text + "\n"));
        requireCompiled(r);
        const Texp texp(r.ir);
        REQUIRE(texp.chunk().exprs().size() == 1);
        std::string got;
        for (ExprOp o : texp.ops(0)) {
            got += std::to_string(static_cast<int>(o)) + " ";
        }
        CAPTURE(got);
        CHECK(texp.ops(0) == c.ops);
    }
    // The delayed port keeps 0.5 s of history; a run-time delay 4 s.
    const CompileResult delayed =
        compileText(docText("    intro #Box translate.y = port(\"hp\").valueAtTime(time - 0.5)\n"
                            "    intro #A translate.y = port(\"hp\").valueAtTime(time - port(\"d\"))\n"));
    requireCompiled(delayed);
    const Texp texp(delayed.ir);
    REQUIRE(texp.chunk().exprs().size() == 2);
    CHECK(texp.chunk().exprs()[0].portHistorySeconds == 0.5f);
    CHECK(texp.chunk().exprs()[1].portHistorySeconds == 4.0f);
    CHECK(texp.chunk().exprs()[0].inputMask == kExprInputPortHistory);
    // The row of another timeline: PushProp of #Other's translate.y.
    const CompileResult other = compileText(docText("    intro #Box translate.y = layer(\"Other\").y\n"));
    const Texp otherTexp(other.ir);
    const IrExprOp& push = otherTexp.chunk().ops()[otherTexp.chunk().exprs()[0].opOffset];
    CHECK(otherTexp.view->string(otherTexp.view->doc().elements()[push.ref].id) == "Other");
    CHECK(push.imm == static_cast<float>(prop::TranslateY * 4));
    CHECK(otherTexp.chunk().exprs()[0].inputMask == kExprInputProp);
}

TEST_CASE("T-777 another timeline's row, the per-play random number and a delayed port at run time") {
    SUBCASE("layer(\"Other\").y follows the other timeline") {
        const CompileResult r = compileText(docText("    intro #Box translate.y = layer(\"Other\").y + 1\n"));
        requireCompiled(r);
        Loaded loaded(r.ir);
        for (float t : {0.0f, 0.5f, 1.5f}) {
            CAPTURE(t);
            REQUIRE(loaded.doc->seekTimeline("", "other", t) == Status::kOk);
            loaded.step();
            const float otherY = loaded.prop("Other", prop::TranslateY);
            CHECK(otherY == doctest::Approx(10.0f + 10.0f * t));
            CHECK(loaded.at(1.0f) == doctest::Approx(otherY + 1.0f));
        }
    }
    SUBCASE("randomPerPlay(0, 10): fixed within a play, in range") {
        const CompileResult r =
            compileText(docText("    intro #Box translate.y = randomPerPlay(0, 10) + value\n"));
        requireCompiled(r);
        Loaded loaded(r.ir);
        const float first = loaded.at(0.0f);
        CHECK(first >= 0.0f);
        CHECK(first < 10.0f);
        for (int k : kSampleFrames) {
            const float t = static_cast<float>(k) / 60.0f;
            CHECK(loaded.at(t) - 20.0f * t == doctest::Approx(first).epsilon(1e-4));
        }
    }
    SUBCASE("port(\"hp\").valueAtTime(time - 0.5) while playing") {
        const CompileResult r =
            compileText(docText("    intro #Box translate.y = port(\"hp\").valueAtTime(time - 0.5)\n"));
        requireCompiled(r);
        Loaded loaded(r.ir);
        loaded.port("hp", 1.0f);
        REQUIRE(loaded.doc->playTimeline("", "intro") == Status::kOk);
        for (int i = 0; i < 40; ++i) {
            loaded.step(1.0f / 60.0f);
        }
        loaded.port("hp", 5.0f);
        for (int i = 0; i < 15; ++i) {
            loaded.step(1.0f / 60.0f);
        }
        CHECK(loaded.prop("Box", prop::TranslateY) == doctest::Approx(1.0f));  // 0.25 s after the change.
        for (int i = 0; i < 24; ++i) {
            loaded.step(1.0f / 60.0f);
        }
        CHECK(loaded.prop("Box", prop::TranslateY) == doctest::Approx(5.0f));  // 0.65 s after.
    }
}

TEST_CASE("T-777 a paused timeline: a port change updates the picture once") {
    const CompileResult r = compileText(docText("    intro #Box translate.y = value * port(\"k\")\n"));
    requireCompiled(r);
    Loaded loaded(r.ir);
    loaded.port("k", 1.0f);
    REQUIRE(loaded.doc->seekTimeline("", "intro", 1.0f) == Status::kOk);
    CHECK(loaded.step() == 1);
    CHECK(loaded.prop("Box", prop::TranslateY) == doctest::Approx(20.0f));
    CHECK(loaded.step() == 0);
    CHECK(loaded.step() == 0);
    loaded.port("k", 2.0f);
    CHECK(loaded.step() == 1);
    CHECK(loaded.prop("Box", prop::TranslateY) == doctest::Approx(40.0f));
    CHECK(loaded.step() == 0);
    loaded.port("k", 2.0f);  // The same value.
    CHECK(loaded.step() == 0);
    loaded.port("hp", 3.0f);  // Another port moves the shared generation: at most one run, same picture.
    CHECK(loaded.step() <= 1);
    CHECK(loaded.prop("Box", prop::TranslateY) == doctest::Approx(40.0f));
    CHECK(loaded.step() == 0);
}

// ---- the same time-only expression down both paths ----------------------------------------------

TEST_CASE("T-777 a time-only expression forced to run time equals its compile-time keys") {
    const char* texts[] = {
        "wiggle(2, 6)",
        "value + Math.sin(time * 3) * 20",
        "random(0, 10)",
        "noise(time * 2) * 30 + value",
        "linear(time, 0.5, 1.5, 0, 50)",
        "easeOut(time, 0, 2, 10, 60)",
        "seedRandom(5, true); random(-10, 10) + value",
        "seedRandom(9); random(0, 4) + value",
        "posterizeTime(5); value + time * 10",
        "valueAtTime(time - 0.3)",
        "clamp(value * 2, 0, 50)",
        "Math.max(value, 20) % 7 + Math.round(time * 3)",
        "layer(\"A\").y * 0.5",
        "wiggle(3, 5, 2, 0.6)",
        "loopOut(\"pingpong\") + wiggle(1, 2)",
        "time > 1 ? degreesToRadians(value) * 10 : Math.abs(Math.cos(time)) * 8",
    };
    for (const char* text : texts) {
        CAPTURE(text);
        const std::string doc = docText(std::string("    intro #Box translate.y = ") + text + "\n");
        const CompileResult lowered = compileText(doc);
        requireCompiled(lowered);
        REQUIRE(outcomeFor(lowered, "Box", "translate.y")->status == ExpressionOutcome::Status::Lowered);
        CHECK(chunkPayload(lowered.ir, "TEXP").empty());
        const CompileResult forced = compileText(doc, /*allAtRunTime=*/true);
        requireCompiled(forced);
        REQUIRE(outcomeFor(forced, "Box", "translate.y")->status == ExpressionOutcome::Status::NeedsRuntime);
        CHECK_FALSE(outcomeFor(forced, "Box", "translate.y")->runtimeInputs);
        REQUIRE_FALSE(chunkPayload(forced.ir, "TEXP").empty());
        Loaded a(lowered.ir);
        Loaded b(forced.ir);
        for (int k : kSampleFrames) {
            const float t = static_cast<float>(k) / 60.0f;
            CAPTURE(t);
            CHECK(std::fabs(a.at(t) - b.at(t)) <= 0.25f + 1e-3f);
        }
    }
}

TEST_CASE("T-777 expressions without run-time inputs still produce only keys and no TEXP") {
    const CompileResult plain = compileText(docText(""));
    requireCompiled(plain);
    const CompileResult r =
        compileText(docText("    intro #Box translate.y = wiggle(2, 6)\n"
                            "    intro #A translate.y = layer(\"Box\").y + 5\n"));
    requireCompiled(r);
    CHECK(chunkPayload(r.ir, "TEXP").empty());
    CHECK(chunkPayload(plain.ir, "TEXP").empty());
    CHECK(outcomeFor(r, "Box", "translate.y")->status == ExpressionOutcome::Status::Lowered);
    CHECK(outcomeFor(r, "A", "translate.y")->status == ExpressionOutcome::Status::Lowered);
    CHECK_FALSE(outcomeFor(r, "Box", "translate.y")->runtimeInputs);
    CHECK(outcomeFor(r, "Box", "translate.y")->ports.empty());
}

// ---- references between run-time rows -----------------------------------------------------------

TEST_CASE("T-777 a row reading a run-time row runs its program (RefValueAt), at most 4 levels deep") {
    std::string text = R"(<xt version="2">
  <ports>
    k: number in
  </ports>
  <body><div id="R1"></div><div id="R2"></div><div id="R3"></div><div id="R4"></div><div id="R5"></div><div id="R6"></div></body>
  <timelines>
    @timeline intro {
      duration: 2s;
      #R1 { 0s { translate: 0px 0px } 2s { translate: 0px 20px } }
      #R2 { 0s { translate: 0px 0px } }
      #R3 { 0s { translate: 0px 0px } }
      #R4 { 0s { translate: 0px 0px } }
      #R5 { 0s { translate: 0px 0px } }
      #R6 { 0s { translate: 0px 0px } }
    }
  </timelines>
  <expressions>
    intro #R1 translate.y = value + port("k")
    intro #R2 translate.y = layer("R1").y + 1
    intro #R3 translate.y = layer("R2").y + 1
    intro #R4 translate.y = layer("R3").y + 1
    intro #R5 translate.y = layer("R4").y + 1
    intro #R6 translate.y = layer("R5").y + 1
  </expressions>
</xt>
)";
    const CompileResult r = compileText(text);
    requireCompiled(r);
    for (const char* id : {"R1", "R2", "R3", "R4", "R5"}) {
        CAPTURE(id);
        CHECK(outcomeFor(r, id, "translate.y")->status == ExpressionOutcome::Status::NeedsRuntime);
    }
    const ExpressionOutcome* r6 = outcomeFor(r, "R6", "translate.y");
    REQUIRE(r6 != nullptr);
    CHECK(r6->status == ExpressionOutcome::Status::Skipped);
    CHECK(r6->runtimeInputs);
    REQUIRE_FALSE(r6->diagnostics.empty());
    CHECK(r6->diagnostics.back().code == "expr.limit");
    CHECK(r6->diagnostics.back().message.find("levels deep") != std::string::npos);
    const Texp texp(r.ir);
    CHECK(texp.chunk().exprs().size() == 5);
    Loaded loaded(r.ir);
    loaded.port("k", 3.0f);
    for (float t : {0.0f, 1.0f, 2.0f}) {
        CAPTURE(t);
        REQUIRE(loaded.doc->seekTimeline("", "intro", t) == Status::kOk);
        loaded.step();
        const float r1 = 10.0f * t + 3.0f;
        CHECK(loaded.prop("R1", prop::TranslateY) == doctest::Approx(r1));
        CHECK(loaded.prop("R3", prop::TranslateY) == doctest::Approx(r1 + 2.0f));
        CHECK(loaded.prop("R5", prop::TranslateY) == doctest::Approx(r1 + 4.0f));
        CHECK(loaded.prop("R6", prop::TranslateY) == doctest::Approx(0.0f));  // Skipped: its keys.
    }
}

// ---- 58 §58.5a: what is still rejected ---------------------------------------------------------

TEST_CASE("T-777 each rejection of 58 §58.5a skips its entry with a note; the others compile") {
    // port + (port + (... 40 deep)) without 40 parentheses (the parser has its own nesting limit).
    const std::string deep =
        "{\n      var a = port(\"k\");\n      for (var i = 0; i < 40; i++) { a = port(\"k\") + a; }\n"
        "      a\n    }";
    struct Case {
        std::string text;
        const char* code;
        const char* needle;
    };
    const Case cases[] = {
        {"{\n      var s = 0;\n      for (var i = 0; i < 300; i++) { s += port(\"k\"); }\n      s\n    }",
         "expr.limit", "operations at run time"},
        {deep, "expr.limit", "stack"},
        {"{\n      var s = 0;\n      for (var i = 0; i < port(\"k\"); i++) { s += 1; }\n      s\n    }",
         "expr.limit", "loop whose count is not known"},
        {"port(\"hp\").valueAtTime(time - 5)", "expr.limit", "longer than 4 seconds"},
        {"layer(\"Label\").width.valueAtTime(time - 1)", "expr.limit", "history"},
        {"port(\"nope\") * 2", "expr.port", "not declared"},
        {"port(\"title\") * 2", "expr.port", "not a number port"},
        // T-781: another timeline's row is read from its history, at most 4 s back.
        {"layer(\"Other\").y.valueAtTime(time - 5)", "expr.limit", "longer than 4 seconds"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        const CompileResult r = compileText(docText("    intro #Box translate.y = " + c.text +
                                                    "\n    intro #A translate.y = value + port(\"k\")\n"));
        requireCompiled(r);
        const ExpressionOutcome* bad = outcomeFor(r, "Box", "translate.y");
        REQUIRE(bad != nullptr);
        CHECK(bad->status == ExpressionOutcome::Status::Skipped);
        const Diagnostic* d = xt::test::v2::findCode(bad->diagnostics, c.code);
        REQUIRE(d != nullptr);
        CAPTURE(d->message);
        CHECK(d->message.find(c.needle) != std::string::npos);
        CHECK(d->level == Diagnostic::Level::Warning);
        CHECK(outcomeFor(r, "A", "translate.y")->status == ExpressionOutcome::Status::NeedsRuntime);
        const Texp texp(r.ir);
        CHECK(texp.chunk().exprs().size() == 1);
    }
}

// ---- T-781: the property history, shapes inside an inline <svg> ---------------------------------

TEST_CASE("T-781 layer(\"Other\").y.valueAtTime(time - d): PushPropDelayed, from the property history") {
    struct Case {
        const char* text;
        std::vector<ExprOp> ops;
        float seconds;
    };
    const Case cases[] = {
        {"layer(\"Other\").y.valueAtTime(time - 0.3)", {ExprOp::PushConst, ExprOp::PushPropDelayed}, 0.3f},
        {"layer(\"Other\").y.valueAtTime(time - port(\"d\")) + 1",
         {ExprOp::PushPort, ExprOp::PushPropDelayed, ExprOp::PushConst, ExprOp::Add},
         4.0f},
        // posterizeTime(): the delay from the quantized time (at most 1 / fps back).
        {"posterizeTime(4); layer(\"Other\").y",
         {ExprOp::PushTime, ExprOp::PushTime, ExprOp::PushConst, ExprOp::Mul, ExprOp::PushConst, ExprOp::Add,
          ExprOp::Floor, ExprOp::PushConst, ExprOp::Div, ExprOp::Sub, ExprOp::PushPropDelayed},
         0.25f},
        {"posterizeTime(4); layer(\"Other\").y.valueAtTime(time - 0.5)",
         {ExprOp::PushTime, ExprOp::PushTime, ExprOp::PushConst, ExprOp::Mul, ExprOp::PushConst, ExprOp::Add,
          ExprOp::Floor, ExprOp::PushConst, ExprOp::Div, ExprOp::PushConst, ExprOp::Sub, ExprOp::Sub,
          ExprOp::PushPropDelayed},
         0.75f},
    };
    for (const Case& c : cases) {
        const std::string text = c.text;
        CAPTURE(text);
        const CompileResult r = compileText(docText("    intro #Box translate.y = " + text + "\n"));
        requireCompiled(r);
        REQUIRE(outcomeFor(r, "Box", "translate.y")->status == ExpressionOutcome::Status::NeedsRuntime);
        const Texp texp(r.ir);
        REQUIRE(texp.chunk().exprs().size() == 1);
        std::string got;
        for (ExprOp o : texp.ops(0)) {
            got += std::to_string(static_cast<int>(o)) + " ";
        }
        CAPTURE(got);
        CHECK(texp.ops(0) == c.ops);
        const IrTimelineExpr& e = texp.chunk().exprs()[0];
        CHECK(e.portHistorySeconds == doctest::Approx(c.seconds));
        CHECK((e.inputMask & kExprInputPropHistory) != 0);
        const IrExprOp& read =
            texp.chunk().ops()[e.opOffset + e.opCount - 1 - (texp.ops(0).back() == ExprOp::Add ? 2 : 0)];
        REQUIRE(read.op == static_cast<uint8_t>(ExprOp::PushPropDelayed));
        CHECK(texp.view->string(texp.view->doc().elements()[read.ref].id) == "Other");
        CHECK(read.imm == static_cast<float>(prop::TranslateY * 4));
    }
}

TEST_CASE(
    "T-781 another timeline's row 0.3 s ago: lags while playing, the oldest record first, no run paused") {
    const CompileResult r =
        compileText(docText("    intro #Box translate.y = layer(\"Other\").y.valueAtTime(time - 0.3)\n"));
    requireCompiled(r);
    Loaded loaded(r.ir);
    const float dt = 1.0f / 60.0f;
    // The reading timeline stopped (seeked), the other one playing: no history, no run.
    REQUIRE(loaded.doc->seekTimeline("", "intro", 1.0f) == Status::kOk);
    REQUIRE(loaded.doc->playTimeline("", "other") == Status::kOk);
    CHECK(loaded.step(dt) <= 1);
    const float held = loaded.prop("Box", prop::TranslateY);
    for (int i = 0; i < 10; ++i) {
        CHECK(loaded.step(dt) == 0);
    }
    CHECK(loaded.prop("Box", prop::TranslateY) == held);
    CHECK(loaded.prop("Other", prop::TranslateY) > 10.0f);  // It did move meanwhile.
    // Both timelines play from 0: Other.y = 10 + 10 t (other), Box reads it 0.3 s late.
    REQUIRE(loaded.doc->stopTimeline("", "other") == Status::kOk);
    REQUIRE(loaded.doc->playTimeline("", "other") == Status::kOk);
    REQUIRE(loaded.doc->playTimeline("", "intro") == Status::kOk);
    std::vector<std::pair<float, float>> seen;  // (seconds since play, Other.y)
    float first = -1;
    for (int i = 1; i <= 90; ++i) {
        loaded.step(dt);
        const float t = static_cast<float>(i) * dt;
        seen.emplace_back(t, loaded.prop("Other", prop::TranslateY));
        const float box = loaded.prop("Box", prop::TranslateY);
        CAPTURE(t);
        if (t < 0.25f) {
            // Not 0.3 s of history yet: the oldest record (Other.y when the first one was taken, at
            // the start of the first update: released by the stop, before `other` wrote again).
            if (i == 1) {
                first = box;
                CHECK(box <= seen.front().second);
            }
            CHECK(box == first);
        } else if (t > 0.4f) {
            // The value 0.3 s ago (one update of lag: the history records before this update's writes).
            CHECK(std::fabs(box - (10.0f + 10.0f * (t - 0.3f))) <= 10.0f * dt + 0.05f);
        }
    }
}

namespace {

constexpr const char* kSvgDoc = R"(<xt version="2">
  <ports>
    p: number in
    k: number in
  </ports>
  <body>
    <svg id="Icon" viewBox="0 0 32 32" width="32" height="32">
      <g id="g"><path id="tri" d="M0 0 L10 0 L10 10 Z" fill="#ff0000"/></g>
      <path id="line" d="M0 16 L32 16" stroke="#000000"/>
    </svg>
  </body>
  <timelines>
    @timeline main {
      duration: 1s;
      #Icon/line { 0s { path-trim-end: 0 } 1s { path-trim-end: 1 } }
      #Icon/g { 0s { rotate: 0deg } 1s { rotate: 90deg } }
    }
  </timelines>
  <expressions>
)";

/// The path / group nodes of the inline <svg> `Icon` (document order).
std::vector<NodeId> svgNodes(Document& doc, NodeKind kind) {
    std::vector<NodeId> out;
    doc.arena().traversePreorder(doc.byId("Icon").node, [&](NodeId n) {
        if (doc.arena().kind(n) == kind) {
            out.push_back(n);
        }
        return true;
    });
    return out;
}

}  // namespace

TEST_CASE("T-781 a run-time expression on a shape inside an inline <svg> runs on its scene track") {
    const CompileResult plain = compileText(std::string(kSvgDoc) + "  </expressions>\n</xt>\n");
    requireCompiled(plain);
    CHECK(chunkPayload(plain.ir, "TEXP").empty());
    const CompileResult r =
        compileText(std::string(kSvgDoc) +
                    "    main #Icon/line path-trim-end = linear(port(\"p\"), 0, 1, 0, 1)\n"
                    "    main #Icon/g rotate = value * port(\"k\")\n"
                    "  </expressions>\n</xt>\n");
    requireCompiled(r);
    const ExpressionOutcome* line = outcomeFor(r, "Icon/line", "path-trim-end");
    REQUIRE(line != nullptr);
    for (const Diagnostic& d : line->diagnostics) {
        CAPTURE(d.message);
    }
    CHECK(line->status == ExpressionOutcome::Status::NeedsRuntime);
    CHECK(outcomeFor(r, "Icon/g", "rotate")->status == ExpressionOutcome::Status::NeedsRuntime);
    const Texp texp(r.ir);
    REQUIRE(texp.chunk().exprs().size() == 2);
    for (const IrTimelineExpr& e : texp.chunk().exprs()) {
        const IrTimelineGroup& g = texp.view->timelineGroups().groups()[e.group];
        const IrTimelineGroupMember& m = texp.view->timelineGroups().members()[g.memberOffset + e.member];
        CHECK(texp.view->string(texp.view->doc().elements()[m.element].id) == "Icon");
        CHECK(e.index == 1.0f);
    }
    Loaded loaded(r.ir);
    const std::vector<NodeId> paths = svgNodes(*loaded.doc, NodeKind::Path);
    const std::vector<NodeId> groups = svgNodes(*loaded.doc, NodeKind::Group);
    REQUIRE(paths.size() == 2);
    REQUIRE(groups.size() >= 2);
    loaded.port("p", 0.25f);
    loaded.port("k", 2.0f);
    REQUIRE(loaded.doc->seekTimeline("", "main", 0.5f) == Status::kOk);
    loaded.step();
    CHECK(loaded.doc->arena().path(paths[1]).trimEnd == doctest::Approx(0.25f));
    CHECK(loaded.doc->arena().common(groups[1]).rotation == doctest::Approx(90.0f).epsilon(0.01));
    CHECK(loaded.step() == 0);
    // Paused (seeked): a port change updates the shape once.
    loaded.port("p", 0.75f);
    CHECK(loaded.step() >= 1);
    CHECK(loaded.doc->arena().path(paths[1]).trimEnd == doctest::Approx(0.75f));
    CHECK(loaded.step() == 0);
    CHECK(loaded.doc->arena().path(paths[1]).trimEnd == doctest::Approx(0.75f));
    // Playing: it follows the port as it changes.
    REQUIRE(loaded.doc->playTimeline("", "main") == Status::kOk);
    loaded.port("p", 0.5f);
    loaded.step(0.1f);
    CHECK(loaded.doc->arena().path(paths[1]).trimEnd == doctest::Approx(0.5f));
}

// ---- the IR ------------------------------------------------------------------------------------

TEST_CASE("T-777 IrWriter::fromView keeps the TEXP chunk byte for byte") {
    const CompileResult r =
        compileText(docText("    intro #Box translate.y = wiggle(2, 6) * port(\"shake\")\n"
                            "    intro #A translate.y = posterizeTime(4); wiggle(1, 3) + "
                            "layer(\"Label\").width\n"));
    requireCompiled(r);
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    REQUIRE(view.value().timelineExprs().present());
    CHECK(IrWriter::fromView(view.value()).finish() == r.ir);
}

TEST_CASE("T-777 noise(x) is a function of x alone, on every row and on both paths (AE)") {
    const CompileResult r =
        compileText(docText("    intro #Box translate.y = noise(time * 3) * 10\n"
                            "    intro #A translate.y = noise(time * 3) * 10\n"));
    requireCompiled(r);
    Loaded loaded(r.ir);
    for (int k : kSampleFrames) {
        const float t = static_cast<float>(k) / 60.0f;
        CHECK(loaded.at(t, "Box") == doctest::Approx(loaded.at(t, "A")));
    }
    // The run-time Noise op seeds with exprMixSeed(seed, ref) == kExprNoiseSeed for any row's seed.
    for (uint32_t seed :
         {0u, 1u, 0x12345678u, 0xFFFFFFFFu, expressionSeed("intro", "Box", "translate", "y")}) {
        CAPTURE(seed);
        CHECK(exprMixSeed(seed, xe::exprNoiseRef(seed)) == 0x4E4F4953u);
    }
}

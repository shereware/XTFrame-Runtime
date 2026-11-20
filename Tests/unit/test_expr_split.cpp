#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "expr/expr_split.h"
#include "expr_test_util.h"

/// T-766 (Docs/tasks/T-766.md, Docs/design/58-value-functions-and-drivers.md §58.5): splitRuntime()
/// expands loops, local functions and arrays into straight-line scalar form and pulls out the
/// maximal time-only sub-expressions. The expanded form must compute what the evaluator computes, for
/// stand-in run-time inputs at many times.

using namespace expr_test;

namespace {

const std::vector<xe::ExprKey> kRow = {key(0, 2, xt::Easing::cssEaseInOut()), key(1.5, 8), key(3, 5)};

struct Inputs {
    double hp = 0.1;
    double scale = 1;
    xe::ExprRuntimeInputs in;
    Inputs() {
        in.port = [this](std::string_view name, double t) {
            if (name == "hp") {
                return hp + 0.05 * std::sin(t * 3);
            }
            if (name == "f") {
                return 1.5 + 0.5 * scale;
            }
            if (name == "d") {
                return 0.25 * scale;
            }
            return scale * (1 + t * 0.1);
        };
        in.layout = [this](const xe::ExprLayerKey& k, int which) {
            return (k.name == "Label" ? 100.0 : 10.0) * scale + which;
        };
        in.playRandom = [this](uint32_t site, double lo, double hi) {
            return lo + (hi - lo) * (0.3 + 0.1 * site) * scale;
        };
        in.layerProp = [this](const xe::ExprLayerKey&, std::string_view, double t) { return 40 * scale + t; };
    }
};

xe::SplitResult split(const std::string& text, const xe::ExprContext& ctx) {
    const xe::Program p = parse(text);
    xe::SplitResult s = xe::splitRuntime(p, ctx);
    INFO("expression: " << text);
    if (!s.diagnostics.empty()) {
        INFO("diagnostic: " << s.diagnostics.front().message);
        CHECK(s.diagnostics.empty());
    }
    REQUIRE(s.status == xe::ExprStatus::Ok);
    return s;
}

/// No curve's subtree holds a run-time leaf; every remainder fits the run-time program limit.
void checkShape(const xe::SplitResult& s) {
    for (const xe::SplitCurve& c : s.curves) {
        CHECK_FALSE(s.nodes[static_cast<size_t>(c.root)].runtime);
        CHECK(s.nodes[static_cast<size_t>(c.root)].timeDep);
    }
    for (size_t ops : s.remainderOps) {
        CHECK(ops <= xe::kSplitMaxOps);
    }
}

/// The split's remainder (with its curves) equals direct evaluation, for several input values.
void checkEquivalent(const std::string& text, int index = 1) {
    Inputs inputs;
    xe::ExprContext ctx = context(kRow, 3, index, 4242);
    ctx.runtimeInputs = &inputs.in;
    ctx.otherRows = [](const xe::ExprLayerKey& k, std::string_view) {
        xe::ExprRowRef r;
        if (k.name == "Other") {
            r.kind = xe::ExprRefKind::Runtime;  // another timeline's row
        }
        return r;
    };
    const xe::Program p = parse(text);
    const xe::SplitResult s = xe::splitRuntime(p, ctx);
    INFO("expression: " << text);
    if (!s.diagnostics.empty()) {
        INFO("diagnostic: " << s.diagnostics.front().message);
        CHECK(s.diagnostics.empty());
    }
    REQUIRE(s.status == xe::ExprStatus::Ok);
    checkShape(s);
    for (double scale : {0.5, 1.0, 2.0}) {
        for (double hp : {0.1, 0.6}) {
            inputs.scale = scale;
            inputs.hp = hp;
            for (double t = 0; t <= 3.0001; t += 0.1) {
                const xe::EvalResult direct = xe::evaluateExpression(p, ctx, t);
                REQUIRE(direct.status == xe::ExprStatus::Ok);
                REQUIRE(static_cast<size_t>(direct.value.size) == s.components.size());
                for (int c = 0; c < direct.value.size; ++c) {
                    double viaSplit = 0;
                    std::vector<xt::compile::Diagnostic> d;
                    REQUIRE(xe::evalSplitComponent(p, s, c, ctx, t, viaSplit, d) == xe::ExprStatus::Ok);
                    INFO("t = " << t << " scale = " << scale << " hp = " << hp << " component " << c);
                    CHECK(viaSplit == doctest::Approx(direct.value.v[c]).epsilon(1e-12));
                }
            }
        }
    }
}

xt::compile::Diagnostic splitError(const std::string& text) {
    xe::ExprContext ctx = context(kRow, 3);
    const xe::Program p = parse(text);
    const xe::SplitResult s = xe::splitRuntime(p, ctx);
    INFO("expression: " << text);
    REQUIRE(s.status == xe::ExprStatus::Error);
    REQUIRE_FALSE(s.diagnostics.empty());
    return s.diagnostics.front();
}

}  // namespace

TEST_CASE("expr split: wiggle(2, 6) * port(\"shake\") is one curve times one input") {
    const xe::SplitResult s = split("wiggle(2, 6) * port(\"shake\")", context(kRow, 3));
    REQUIRE(s.components.size() == 1);
    REQUIRE(s.curves.size() == 1);
    CHECK(s.nodes[static_cast<size_t>(s.curves[0].root)].op == xe::SplitOp::Wiggle);
    const xe::SplitNode& root = s.nodes[static_cast<size_t>(s.components[0])];
    CHECK(root.op == xe::SplitOp::Mul);
    CHECK(s.remainderOps[0] == 3);  // Mul(CurveRef, Port)
    checkEquivalent("wiggle(2, 6) * port(\"shake\")");
}

TEST_CASE("expr split: the run-time forms of 58 §58.5 compute what the evaluator computes") {
    for (const char* text : {
             "value * port(\"k\")",
             "linear(port(\"hp\"), 0, 1, 20, 200)",
             "port(\"hp\") < 0.2 ? wiggle(8, 4) : value",
             "layer(\"Label\").width + 24",
             "layer(\"Label\").height * 0.5 - layer(\"Icon\").left",
             "randomPerPlay(0, 10) + randomPerPlay()",
             "wiggle(port(\"f\"), 6)",
             "valueAtTime(time - port(\"d\"))",
             "port(\"hp\").valueAtTime(time - 0.5)",
             "var s = 0; for (var i = 0; i < 3; i++) { s += port(\"a\") * i + wiggle(i + 1, 2); } s",
             "function f(x) { return x * 2 + wiggle(1, 2); } f(port(\"a\")) + f(1)",
             "[wiggle(2, 6), 0][0] * port(\"k\")",
             "var v = [port(\"a\"), time]; (v * 2 + [1, 1])[1] + v[0]",
             "var x = 0; if (port(\"hp\") > 0.3) { x = wiggle(2, 3); } else { x = time; } x",
             "port(\"hp\") > 0.3 && port(\"a\") || 7",
             "clamp(port(\"a\") * 10, 0, value) + ease(port(\"hp\"), 0, 1, 1, 9)",
             "Math.max(port(\"a\"), Math.sin(time), 0.5) + Math.pow(port(\"a\"), 2) + length([port(\"a\"), "
             "3])",
             "posterizeTime(8); wiggle(2, 6) + port(\"a\")",
             "seedRandom(4, true); random(port(\"a\")) + random(1, 2) + random()",
             "random(0, port(\"a\")) + random()",
             "loopOut(\"pingpong\") * port(\"a\")",
             "noise(time * port(\"f\")) + noise(time)",
             "var k = index * 2; for (var j = 0; j < k; j++) { k += 0; } k * port(\"a\")",
             "var s = 0; for (var i = 0; i < time * 4; i++) { s += 1; } s * port(\"k\")",
             "var w = 0; while (w < 10) { w += 3; if (w > 5) break; } w + port(\"a\")",
             "layer(\"Other\").y + port(\"a\")",
         }) {
        checkEquivalent(text, 3);
    }
}

TEST_CASE("expr split: constants fold, time-only parts become curves") {
    const xe::ExprContext ctx = context(kRow, 3, 2);
    {
        const xe::SplitResult s = split("(index + 1) * 2 + port(\"a\")", ctx);
        CHECK(s.curves.empty());
        CHECK(s.remainderOps[0] == 3);  // Add(Const 6, Port)
    }
    {
        const xe::SplitResult s = split("wiggle(2, 6) + 1", ctx);
        REQUIRE(s.curves.size() == 1);  // no run-time input: the whole result is one curve
        CHECK(s.remainderOps[0] == 1);
        CHECK(s.nodes[static_cast<size_t>(s.components[0])].op == xe::SplitOp::CurveRef);
    }
    {
        // value alone stays a curve of its own (T-777 emits PushValue for ValueAt(Time)).
        const xe::SplitResult s = split("value * port(\"k\")", ctx);
        REQUIRE(s.curves.size() == 1);
        const xe::SplitNode& c = s.nodes[static_cast<size_t>(s.curves[0].root)];
        CHECK(c.op == xe::SplitOp::ValueAt);
        CHECK(s.nodes[static_cast<size_t>(c.args[0])].op == xe::SplitOp::Time);
    }
    {
        // Arrays split into components.
        const xe::SplitResult s = split("[port(\"a\"), wiggle(1, 1), 3]", ctx);
        CHECK(s.isArray);
        REQUIRE(s.components.size() == 3);
        CHECK(s.nodes[static_cast<size_t>(s.components[0])].op == xe::SplitOp::Port);
        CHECK(s.nodes[static_cast<size_t>(s.components[1])].op == xe::SplitOp::CurveRef);
        CHECK(s.nodes[static_cast<size_t>(s.components[2])].op == xe::SplitOp::Const);
    }
    {
        // A loop whose count depends on time becomes a sub-program curve.
        const xe::SplitResult s = split("var n = 0; while (n < time * 3) { n += 1; } n * port(\"k\")", ctx);
        REQUIRE(s.curves.size() == 1);
        CHECK(s.nodes[static_cast<size_t>(s.curves[0].root)].op == xe::SplitOp::SubProgram);
    }
}

TEST_CASE("expr split: a curve evaluates on its own") {
    const xe::ExprContext ctx = context(kRow, 3, 1, 4242);
    const xe::Program p = parse("wiggle(2, 6) * port(\"shake\")");
    const xe::SplitResult s = xe::splitRuntime(p, ctx);
    REQUIRE(s.status == xe::ExprStatus::Ok);
    const xe::Program w = parse("wiggle(2, 6)");
    for (double t = 0; t <= 3; t += 0.25) {
        double viaCurve = 0;
        std::vector<xt::compile::Diagnostic> d;
        REQUIRE(xe::evalSplitCurve(p, s, 0, ctx, t, viaCurve, d) == xe::ExprStatus::Ok);
        CHECK(viaCurve == doctest::Approx(xe::evaluateExpression(w, ctx, t).value.v[0]).epsilon(1e-12));
    }
    // Without stand-in inputs the remainder needs run time.
    double out = 0;
    std::vector<xt::compile::Diagnostic> d;
    CHECK(xe::evalSplitComponent(p, s, 0, ctx, 1, out, d) == xe::ExprStatus::NeedsRuntime);
}

TEST_CASE("expr split: what cannot be expanded is an error that says why") {
    struct Case {
        const char* text;
        const char* code;
        const char* contains;
    };
    const Case cases[] = {
        {"var s = 0; while (s < port(\"a\")) { s += 1; } s", "expr.limit", "not known at compile time"},
        {"var s = 0; for (var i = 0; i < 200; i++) { s += port(\"a\") * i; } s", "expr.limit",
         "the limit is 256"},
        {"var s = 0; for (var i = 0; i < 5; i++) { if (port(\"a\") > i) break; s += 1; } s", "expr.limit",
         "break / continue"},
        {"function f(x) { if (x > 0) { return 1; } return 2; } f(port(\"a\"))", "expr.limit", "return under"},
        {"if (port(\"a\") > 0) { 5 }", "expr.limit", "only one branch"},
        {"var y; if (port(\"a\") > 0) { y = 1; } y + 1", "expr.limit", "only one branch"},
        {"[1, 2][port(\"a\")]", "expr.limit", "known at compile time"},
        {"layer(port(\"a\")).width", "expr.limit", "known at compile time"},
        {"if (port(\"a\") > 0) { seedRandom(2); } random()", "expr.limit", "seedRandom"},
        {"var t = 0; if (time > 1) { for (var i = 0; i < time; i++) { t += 1; } } t * port(\"a\")",
         "expr.limit", "inside a function or a condition"},
        {"var s = 0; for (var i = 0; i < time; i++) { s += port(\"a\"); } s", "expr.limit",
         "the loop reads one"},
        {"var a = port(\"a\"); var s = 0; for (var i = 0; i < time; i++) { s += a; } s", "expr.limit",
         "derived from one"},
        {"layer(\"Logo\").y", "expr.ref", "not supported here"},
    };
    for (const Case& c : cases) {
        const xt::compile::Diagnostic d = splitError(c.text);
        INFO("expression: " << c.text << " -> " << d.code << ": " << d.message);
        CHECK(d.code == c.code);
        CHECK(d.message.find(c.contains) != std::string::npos);
    }
}

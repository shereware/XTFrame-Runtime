#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "doctest.h"
#include "expr_test_util.h"
#include "xt/expr_noise.h"

/// T-766 (Docs/tasks/T-766.md, Docs/design/58-value-functions-and-drivers.md §58.2): the values of
/// the built-in functions, checked against AE's definitions; wiggle / random determinism; the row
/// sampler is the runtime's own track interpolation; evaluation errors are diagnostics.

using namespace expr_test;

namespace {

constexpr double kPi = 3.14159265358979323846;
const std::vector<xe::ExprKey> kFlat = {key(0, 10)};

}  // namespace

TEST_CASE("expr eval: the row sampler is xt::evalTrack") {
    const std::vector<xe::ExprKey> keys = {key(0, 0, xt::Easing::bezier(0.42f, 0, 0.58f, 1)), key(1, 100),
                                           key(2, 40, xt::Easing::hold()),
                                           key(3, 70, xt::Easing::steps(4, true)), key(4, 10)};
    xt::Timeline tl;
    for (const xe::ExprKey& k : keys) {
        xt::Keyframe kf;
        kf.time = static_cast<float>(k.time);
        kf.value = xt::Value::ofFloat(static_cast<float>(k.value));
        kf.easing = k.easing;
        tl.keyframes.push_back(kf);
    }
    xt::Track tr;
    tr.type = xt::ValueType::Float;
    tr.kfCount = static_cast<uint32_t>(tl.keyframes.size());
    tl.tracks.push_back(tr);
    const xe::ExprContext ctx = context(keys, 5);
    for (double t = -0.5; t <= 4.5; t += 0.1) {
        const float expected = xt::evalTrack(tl, tl.tracks[0], static_cast<float>(t), nullptr, nullptr).f;
        CHECK(eval("value", ctx, t) == static_cast<double>(expected));  // bit for bit
        CHECK(eval("valueAtTime(" + std::to_string(t) + ")", ctx, 0) ==
              static_cast<double>(xt::evalTrack(tl, tl.tracks[0],
                                                static_cast<float>(std::stod(std::to_string(t))), nullptr,
                                                nullptr)
                                      .f));
    }
}

TEST_CASE("expr eval: valueAtTime outside the keys holds the end values") {
    const std::vector<xe::ExprKey> keys = {key(1, 5), key(2, 7)};
    const xe::ExprContext ctx = context(keys, 3);
    CHECK(eval("valueAtTime(-3)", ctx, 0) == doctest::Approx(5));
    CHECK(eval("valueAtTime(10)", ctx, 0) == doctest::Approx(7));
    CHECK(eval("valueAtTime(1.5)", ctx, 0) == doctest::Approx(6));
}

TEST_CASE("expr eval: linear and ease follow AE") {
    const xe::ExprContext ctx = context(kFlat, 4);
    // Clamped outside [tMin, tMax].
    CHECK(eval("linear(time, 1, 2, 10, 20)", ctx, 0.5) == doctest::Approx(10));
    CHECK(eval("linear(time, 1, 2, 10, 20)", ctx, 1.5) == doctest::Approx(15));
    CHECK(eval("linear(time, 1, 2, 10, 20)", ctx, 3) == doctest::Approx(20));
    CHECK(eval("linear(1.5, 2, 1, 10, 20)", ctx, 0) == doctest::Approx(15));  // bounds swapped
    CHECK(eval("linear(0.25, 0, 100)", ctx, 0) == doctest::Approx(25));       // 3-argument form
    CHECK(eval("linear(0.5, 0, 1, [0, 10], [10, 30])[1]", ctx, 0) == doctest::Approx(20));
    // ease: the AE curve, through xt::evalEasing.
    const double e =
        100.0 * static_cast<double>(xt::evalEasing(xt::Easing::bezier(xt::kExprEase[0], xt::kExprEase[1],
                                                                      xt::kExprEase[2], xt::kExprEase[3]),
                                                   0.3f));
    CHECK(eval("ease(0.3, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(e));
    CHECK(eval("ease(0, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(0));
    CHECK(eval("ease(1, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(100));
    CHECK(eval("ease(0.5, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(50).epsilon(0.02));
    CHECK(eval("ease(0.1, 0, 1, 0, 100)", ctx, 0) < 10);  // slow start
    CHECK(eval("easeIn(0.5, 0, 1, 0, 100)", ctx, 0) < 50);
    CHECK(eval("easeOut(0.5, 0, 1, 0, 100)", ctx, 0) > 50);
    CHECK(eval("easeIn(-1, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(0));
    CHECK(eval("easeOut(5, 0, 1, 0, 100)", ctx, 0) == doctest::Approx(100));
    double prev = -1;
    for (int i = 0; i <= 20; ++i) {
        const double v = eval("ease(" + std::to_string(i / 20.0) + ", 0, 1, 0, 100)", ctx, 0);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST_CASE("expr eval: clamp, Math and angle conversions") {
    const xe::ExprContext ctx = context(kFlat, 1);
    CHECK(eval("clamp(5, 0, 3)", ctx, 0) == doctest::Approx(3));
    CHECK(eval("clamp(-5, 3, 0)", ctx, 0) == doctest::Approx(0));
    CHECK(eval("clamp([5, -1], 0, 3)[1]", ctx, 0) == doctest::Approx(0));
    CHECK(eval("Math.sin(Math.PI / 2)", ctx, 0) == doctest::Approx(1));  // radians, as in AE
    CHECK(eval("Math.cos(Math.PI)", ctx, 0) == doctest::Approx(-1));
    CHECK(eval("Math.tan(Math.PI / 4)", ctx, 0) == doctest::Approx(1));
    CHECK(eval("Math.asin(1)", ctx, 0) == doctest::Approx(kPi / 2));
    CHECK(eval("Math.acos(1)", ctx, 0) == doctest::Approx(0));
    CHECK(eval("Math.atan(1)", ctx, 0) == doctest::Approx(kPi / 4));
    CHECK(eval("Math.atan2(1, 1)", ctx, 0) == doctest::Approx(kPi / 4));
    CHECK(eval("Math.abs(-2)", ctx, 0) == doctest::Approx(2));
    CHECK(eval("Math.round(2.5)", ctx, 0) == doctest::Approx(3));
    CHECK(eval("Math.round(-2.5)", ctx, 0) == doctest::Approx(-2));  // JavaScript rounds halves up
    CHECK(eval("Math.floor(-1.5)", ctx, 0) == doctest::Approx(-2));
    CHECK(eval("Math.ceil(1.2)", ctx, 0) == doctest::Approx(2));
    CHECK(eval("Math.min(3, 1, 2)", ctx, 0) == doctest::Approx(1));
    CHECK(eval("Math.max(3, 1, 2)", ctx, 0) == doctest::Approx(3));
    CHECK(eval("Math.pow(2, 10)", ctx, 0) == doctest::Approx(1024));
    CHECK(eval("Math.sqrt(16)", ctx, 0) == doctest::Approx(4));
    CHECK(eval("Math.exp(0)", ctx, 0) == doctest::Approx(1));
    CHECK(eval("Math.log(Math.exp(2))", ctx, 0) == doctest::Approx(2));
    CHECK(eval("degreesToRadians(180)", ctx, 0) == doctest::Approx(kPi));
    CHECK(eval("radiansToDegrees(Math.PI / 2)", ctx, 0) == doctest::Approx(90));
}

TEST_CASE("expr eval: loopOut's four types on the 2nd and 3rd cycles") {
    const std::vector<xe::ExprKey> keys = {key(0, 0), key(1, 10), key(2, 4)};
    const xe::ExprContext ctx = context(keys, 8);
    // Inside the keys: the row itself.
    CHECK(eval("loopOut(\"cycle\")", ctx, 0.5) == doctest::Approx(5));
    // cycle (period 2): 2nd cycle [2, 4], 3rd [4, 6].
    CHECK(eval("loopOut(\"cycle\")", ctx, 3) == doctest::Approx(10));
    CHECK(eval("loopOut()", ctx, 4.5) == doctest::Approx(5));
    CHECK(eval("loopOut(\"cycle\")", ctx, 5.5) == doctest::Approx(7));
    // pingpong: the 2nd cycle runs backwards, the 3rd forwards.
    CHECK(eval("loopOut(\"pingpong\")", ctx, 2.5) == doctest::Approx(7));
    CHECK(eval("loopOut(\"pingpong\")", ctx, 3.5) == doctest::Approx(5));
    CHECK(eval("loopOut(\"pingpong\")", ctx, 4.5) == doctest::Approx(5));
    CHECK(eval("loopOut(\"pingpong\")", ctx, 5.5) == doctest::Approx(7));
    // offset: each cycle adds (last - first) = 4.
    CHECK(eval("loopOut(\"offset\")", ctx, 3) == doctest::Approx(14));
    CHECK(eval("loopOut(\"offset\")", ctx, 5) == doctest::Approx(18));
    CHECK(eval("loopOut(\"offset\")", ctx, 5.5) == doctest::Approx(15));
    // continue: the slope arriving at the last key (-6 per second).
    CHECK(eval("loopOut(\"continue\")", ctx, 3) == doctest::Approx(-2));
    CHECK(eval("loopOut(\"continue\")", ctx, 5) == doctest::Approx(-14));
    // numKeyframes = 1: only the last segment loops.
    CHECK(eval("loopOut(\"cycle\", 1)", ctx, 2.5) == doctest::Approx(7));
    CHECK(eval("loopOut(\"cycle\", 1)", ctx, 3.5) == doctest::Approx(7));
}

TEST_CASE("expr eval: loopIn's four types before the first key") {
    const std::vector<xe::ExprKey> keys = {key(4, 0), key(5, 10), key(6, 4)};
    const xe::ExprContext ctx = context(keys, 8);
    CHECK(eval("loopIn(\"cycle\")", ctx, 3.5) == doctest::Approx(7));  // 2nd cycle [2, 4]
    CHECK(eval("loopIn(\"cycle\")", ctx, 1.5) == doctest::Approx(7));  // 3rd cycle [0, 2]
    CHECK(eval("loopIn(\"pingpong\")", ctx, 3.5) == doctest::Approx(5));
    CHECK(eval("loopIn(\"pingpong\")", ctx, 1.5) == doctest::Approx(7));
    CHECK(eval("loopIn(\"offset\")", ctx, 3.5) == doctest::Approx(3));
    CHECK(eval("loopIn(\"offset\")", ctx, 1.5) == doctest::Approx(-1));
    CHECK(eval("loopIn(\"continue\")", ctx, 3.5) == doctest::Approx(-5));
    CHECK(eval("loopIn(\"continue\")", ctx, 4.5) == doctest::Approx(5));  // inside: the row
}

TEST_CASE("expr eval: loopOut continue with an eased last segment uses its end tangent") {
    // ease-out arrives flat: zero slope.
    const std::vector<xe::ExprKey> keys = {key(0, 0, xt::Easing::bezier(0, 0, 0.58f, 1)), key(1, 10)};
    CHECK(eval("loopOut(\"continue\")", context(keys, 3), 2) == doctest::Approx(10));
    // A cubic bezier (0.2, 0, 0.5, 0.5) arrives with slope (1 - 0.5) / (1 - 0.5) = 1 x the chord.
    const std::vector<xe::ExprKey> k2 = {key(0, 0, xt::Easing::bezier(0.2f, 0, 0.5f, 0.5f)), key(1, 10)};
    CHECK(eval("loopOut(\"continue\")", context(k2, 3), 2) == doctest::Approx(20));
}

TEST_CASE("expr eval: wiggle is deterministic, seamless and bounded") {
    const xe::ExprContext a = context(kFlat, 15, 1, 77);
    const xe::ExprContext b = context(kFlat, 15, 1, 78);
    CHECK(eval("wiggle(2, 6)", a, 3.3) == eval("wiggle(2, 6)", a, 3.3));
    CHECK(eval("wiggle(2, 6)", a, 3.3) != eval("wiggle(2, 6)", b, 3.3));
    // Seamless loop: time 0 and the duration give the same value.
    CHECK(eval("wiggle(2, 6)", a, 0) == eval("wiggle(2, 6)", a, 15));
    CHECK(eval("wiggle(0.37, 6)", a, 0) == eval("wiggle(0.37, 6)", a, 15));
    CHECK(eval("wiggle(2, 6, 3, 0.5)", a, 0) == eval("wiggle(2, 6, 3, 0.5)", a, 15));
    // Two call sites in one expression are independent.
    CHECK(eval("wiggle(2, 6) - wiggle(2, 6)", a, 3.3) != 0);
    // Amplitude: |wiggle - value| <= amp, and the range is used.
    double maxDev = 0;
    double sum = 0;
    int n = 0;
    const xe::Program p = parse("wiggle(2, 6)");
    for (double t = 0; t <= 15; t += 1.0 / 60) {
        const xe::EvalResult r = xe::evaluateExpression(p, a, t);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        const double dev = r.value.v[0] - 10;
        CHECK(std::fabs(dev) <= 6.0 + 1e-9);
        maxDev = std::max(maxDev, std::fabs(dev));
        sum += dev;
        ++n;
    }
    CHECK(maxDev > 3.0);
    CHECK(std::fabs(sum / n) < 3.0);
    // Continuous: neighbouring samples are close.
    CHECK(std::fabs(eval("wiggle(2, 6)", a, 5.0) - eval("wiggle(2, 6)", a, 5.001)) < 0.2);
    // wiggle(freq, amp, octaves, amp_mult, t) samples the row at t.
    const std::vector<xe::ExprKey> ramp = {key(0, 0), key(10, 100)};
    const xe::ExprContext r = context(ramp, 10, 1, 77);
    CHECK(eval("wiggle(1, 0, 1, 0.5, 5)", r, 0) == doctest::Approx(50));
}

TEST_CASE("expr eval: random differs per frame until seedRandom(n, true)") {
    const xe::ExprContext ctx = context(kFlat, 2, 1, 9);
    const double r0 = eval("random()", ctx, 0);
    const double r1 = eval("random()", ctx, 1.0 / 60);
    CHECK(r0 != r1);
    CHECK(eval("random()", ctx, 0) == r0);  // same frame, same number
    CHECK(eval("random() - random()", ctx, 0.5) != 0);
    const double s0 = eval("seedRandom(5, true); random()", ctx, 0);
    CHECK(eval("seedRandom(5, true); random()", ctx, 1.7) == s0);
    CHECK(eval("seedRandom(6, true); random()", ctx, 0) != s0);
    CHECK(eval("seedRandom(5); random()", ctx, 0) != eval("seedRandom(5); random()", ctx, 0.5));
    for (int i = 0; i < 50; ++i) {
        const double t = i / 60.0;
        const double a = eval("random()", ctx, t);
        CHECK(a >= 0);
        CHECK(a < 1);
        const double m = eval("random(10)", ctx, t);
        CHECK(m >= 0);
        CHECK(m < 10);
        const double mm = eval("random(2, 4)", ctx, t);
        CHECK(mm >= 2);
        CHECK(mm < 4);
        const double v = eval("random([1, 100])[1]", ctx, t);
        CHECK(v >= 0);
        CHECK(v < 100);
    }
}

TEST_CASE("expr eval: noise is continuous and bounded") {
    const xe::ExprContext ctx = context(kFlat, 1);
    for (double x = -3; x < 3; x += 0.37) {
        const double a = eval("noise(" + std::to_string(x) + ")", ctx, 0);
        CHECK(a >= -1);
        CHECK(a <= 1);
        const double b = eval("noise(" + std::to_string(x + 1e-4) + ")", ctx, 0);
        CHECK(std::fabs(a - b) < 0.01);
    }
    CHECK(eval("noise(time)", ctx, 0.5) == eval("noise(0.5)", ctx, 0.9));
}

TEST_CASE("expr eval: posterizeTime rounds the time that follows it") {
    const std::vector<xe::ExprKey> ramp = {key(0, 0), key(1, 100)};
    const xe::ExprContext ctx = context(ramp, 1);
    CHECK(eval("posterizeTime(2); time", ctx, 0.7) == doctest::Approx(0.5));
    CHECK(eval("posterizeTime(4); value", ctx, 0.6) == doctest::Approx(50));
    CHECK(eval("var t0 = time; posterizeTime(2); t0", ctx, 0.7) == doctest::Approx(0.7));
    CHECK(eval("posterizeTime(0); time", ctx, 0.7) == doctest::Approx(0));
    CHECK(eval("posterizeTime(2); valueAtTime(0.7)", ctx, 0.9) ==
          doctest::Approx(70));  // explicit times are not rounded
}

TEST_CASE("expr eval: valueVec and array results") {
    const std::vector<xe::ExprKey> xs = {key(0, 1), key(1, 3)};
    const std::vector<xe::ExprKey> ys = {key(0, 10), key(1, 30)};
    const std::vector<xe::ExprRow> rows = {xe::ExprRow{xs, {}}, xe::ExprRow{ys, {}}};
    xe::ExprContext ctx = context(ys, 1);
    ctx.vecRows = rows;
    ctx.component = 1;
    CHECK(eval("valueVec[0] + value", ctx, 0.5) == doctest::Approx(22));
    CHECK(eval("valueVec * 2", ctx, 0.5) == doctest::Approx(40));
    CHECK(eval("length(valueVec)", ctx, 0) == doctest::Approx(std::sqrt(101.0)));
}

TEST_CASE("expr eval: evaluation errors are diagnostics") {
    const xe::ExprContext ctx = context(kFlat, 1);
    struct Case {
        const char* text;
        const char* code;
        const char* contains;
    };
    const Case cases[] = {
        {"1 / 0", "expr.eval", "division by zero"},
        {"5 % 0", "expr.eval", "division by zero"},
        {"Math.sqrt(-1)", "expr.eval", "Math.sqrt"},
        {"Math.log(0)", "expr.eval", "Math.log"},
        {"Math.pow(10, 400)", "expr.eval", "Math.pow"},
        {"[1, 2] * [3, 4]", "expr.eval", "arrays"},
        {"1 + [1, 2]", "expr.eval", "arrays"},
        {"var a; a + 1", "expr.eval", "before it is set"},
        {"if ([1, 2]) 1; else 2", "expr.eval", "array cannot be a condition"},
        {"[1, 2][2]", "expr.eval", "outside the array"},
        {"[1, 2][0.5]", "expr.eval", "outside the array"},
        {"(5).length", "expr.eval", ".length needs an array"},
        {"Math.sin([1, 2])", "expr.eval", "not an array"},
        {"posterizeTime(-1); 1", "expr.eval", "negative"},
        {"var i = 0; while (true) { i++; } i", "expr.limit", "10000"},
        {"var s = 0; for (var i = 0; i < 20000; i++) s++; s", "expr.limit", "10000"},
        {"posterizeTime(12)", "expr.eval", "produces no value"},
        {"function f() { var x = 1; } f() + 1", "expr.eval", "before it is set"},
        {"valueVec", "expr.eval", "valueVec needs"},
        {"layer(\"Logo\").y", "expr.ref", "not supported here"},
        {"1e308 * 10", "expr.eval", "not a finite number"},
    };
    for (const Case& c : cases) {
        const xt::compile::Diagnostic d = evalError(c.text, ctx, 0.25);
        INFO("expression: " << c.text << " -> " << d.code << ": " << d.message);
        CHECK(d.code == c.code);
        CHECK(d.message.find(c.contains) != std::string::npos);
    }
    // An array result without the row's component.
    xe::ExprContext z = ctx;
    z.component = 2;
    const xe::Program p = parse("[1, 2]");
    const xe::EvalResult r = xe::evaluateExpression(p, z, 0);
    double out = 0;
    std::vector<xt::compile::Diagnostic> d;
    CHECK_FALSE(xe::componentOf(r.value, 2, out, d));
    REQUIRE_FALSE(d.empty());
    CHECK(d.front().message.find("no element 2") != std::string::npos);
}

TEST_CASE("expr eval: run-time leaves need run time, or stand-in inputs") {
    const xe::ExprContext ctx = context(kFlat, 1);
    const xe::Program p = parse("port(\"hp\") * 2");
    CHECK(xe::evaluateExpression(p, ctx, 0).status == xe::ExprStatus::NeedsRuntime);
    xe::ExprRuntimeInputs in;
    in.port = [](std::string_view name, double t) { return name == "hp" ? 0.25 + t : 0.0; };
    in.layout = [](const xe::ExprLayerKey& k, int which) {
        return k.name == "Label" && which == 2 ? 120.0 : 0.0;
    };
    in.playRandom = [](uint32_t, double lo, double hi) { return (lo + hi) / 2; };
    xe::ExprContext with = ctx;
    with.runtimeInputs = &in;
    CHECK(eval("port(\"hp\") * 2", with, 0.5) == doctest::Approx(1.5));
    CHECK(eval("port(\"hp\").valueAtTime(time - 0.5)", with, 1.0) == doctest::Approx(0.75));
    CHECK(eval("layer(\"Label\").width + 24", with, 0) == doctest::Approx(144));
    CHECK(eval("randomPerPlay(0, 10)", with, 0) == doctest::Approx(5));
}

TEST_CASE("expr eval: references to other rows go through the resolver") {
    const std::vector<xe::ExprKey> logo = {key(0, 0), key(1, 50)};
    xe::ExprContext ctx = context(kFlat, 1);
    ctx.otherRows = [&](const xe::ExprLayerKey& k, std::string_view prop) {
        xe::ExprRowRef r;
        if (!k.byIndex && k.name == "Logo" && prop == "y") {
            r.kind = xe::ExprRefKind::Row;
            r.row = xe::ExprRow{logo, {}};
        } else if (k.byIndex && k.index == 2) {
            r.kind = xe::ExprRefKind::Runtime;
        }
        return r;
    };
    CHECK(eval("layer(\"Logo\").y", ctx, 0.5) == doctest::Approx(25));
    CHECK(eval("layer(\"Logo\").y.valueAtTime(time - 0.25)", ctx, 0.75) == doctest::Approx(25));
    CHECK(evalError("layer(\"Nope\").y", ctx, 0).code == "expr.ref");
    const xe::Program p = parse("layer(index + 1).x");
    CHECK(xe::evaluateExpression(p, ctx, 0).status == xe::ExprStatus::NeedsRuntime);
}

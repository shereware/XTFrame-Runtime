#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "expr/expr_split.h"
#include "expr_test_util.h"

/// T-766 (Docs/tasks/T-766.md, Docs/design/58-value-functions-and-drivers.md §58.3): lowering to
/// keyframes -- the exact cases (loops, shift, linear in time), sampling plus simplification within
/// the tolerance, Hold keys under posterizeTime, errors and run-time leaves, a fuzz pass and the
/// compile time of 66 expressions over 15 s.

using namespace expr_test;

namespace {

const std::vector<xe::ExprKey> kFlat = {key(0, 10)};

/// Every key time inside [0, duration], strictly increasing.
void checkKeyTimes(const xe::LowerResult& r, double duration) {
    REQUIRE_FALSE(r.keys.empty());
    for (size_t i = 0; i < r.keys.size(); ++i) {
        CHECK(r.keys[i].time >= 0);
        CHECK(r.keys[i].time <= duration + 1e-12);
        if (i > 0) {
            CHECK(r.keys[i].time > r.keys[i - 1].time);
        }
    }
}

/// The lowered keys, played like the runtime, stay within `tol` of direct evaluation at every
/// sample time (except within `skip` seconds of the times in `jumps`).
void checkAgainstDirect(const std::string& text, const xe::ExprContext& ctx, const xe::LowerResult& r,
                        double tol, const std::vector<double>& jumps = {}) {
    const LoweredTrack track(r.keys);
    const xe::Program p = parse(text);
    const size_t n = static_cast<size_t>(std::ceil(ctx.duration * 60 - 1e-9));
    for (size_t k = 0; k <= n; ++k) {
        const double t = k == n ? ctx.duration : static_cast<double>(k) / 60;
        bool skip = false;
        for (double j : jumps) {
            skip = skip || std::fabs(t - j) <= 0.002;
        }
        if (skip) {
            continue;
        }
        const xe::EvalResult e = xe::evaluateExpression(p, ctx, t);
        REQUIRE(e.status == xe::ExprStatus::Ok);
        double direct = 0;
        std::vector<xt::compile::Diagnostic> d;
        REQUIRE(xe::componentOf(e.value, ctx.component, direct, d));
        INFO(text << " at t = " << t);
        CHECK(std::fabs(track.at(t) - direct) <= tol);
    }
}

}  // namespace

TEST_CASE("expr lower: the tolerance table") {
    CHECK(xe::exprTolerance(xe::ExprPropertyKind::Position) == 0.25);
    CHECK(xe::exprTolerance(xe::ExprPropertyKind::Angle) == 0.1);
    CHECK(xe::exprTolerance(xe::ExprPropertyKind::Other) == 0.002);
}

TEST_CASE("expr lower: loopOut(\"cycle\") repeats the keys and keeps their easing") {
    const xt::Easing ease = xt::Easing::bezier(0.42f, 0, 0.58f, 1);
    const std::vector<xe::ExprKey> keys = {key(0, 0, ease), key(1, 10, ease), key(2, 0)};
    const xe::ExprContext ctx = context(keys, 6);
    const xe::LowerResult r = lower("loopOut(\"cycle\")", ctx);
    REQUIRE(r.status == xe::ExprStatus::Ok);
    CHECK(r.method == xe::LowerMethod::ExactLoop);
    REQUIRE(r.keys.size() == 7);
    for (size_t i = 0; i < 7; ++i) {
        CHECK(r.keys[i].time == doctest::Approx(static_cast<double>(i)));
        CHECK(r.keys[i].value == doctest::Approx(i % 2 == 0 ? 0.0 : 10.0));
        if (i < 6) {
            CHECK(r.keys[i].interp == xe::LoweredInterp::Easing);
            CHECK(r.keys[i].easing.kind == xt::EasingKind::CubicBezier);
            CHECK(r.keys[i].easing.x1 == 0.42f);
        }
    }
    checkAgainstDirect("loopOut(\"cycle\")", ctx, r, 1e-4);
}

TEST_CASE("expr lower: loopOut(\"cycle\") with a jump back to the first value") {
    const std::vector<xe::ExprKey> keys = {key(0, 0), key(1, 10)};
    const xe::ExprContext ctx = context(keys, 3.5);
    const xe::LowerResult r = lower("loopOut(\"cycle\")", ctx);
    REQUIRE(r.status == xe::ExprStatus::Ok);
    CHECK(r.method == xe::LowerMethod::ExactLoop);
    checkKeyTimes(r, 3.5);
    // 0 / 1(hold) / 1.001 / 2(hold) / 2.001 / 3(hold) / 3.001 / 3.5 (cut, linear)
    REQUIRE(r.keys.size() == 8);
    CHECK(r.keys[1].interp == xe::LoweredInterp::Hold);
    CHECK(r.keys[2].time == doctest::Approx(1.001));
    CHECK(r.keys[2].value == doctest::Approx(0.01));  // the segment is cut at the jump, not stretched
    CHECK(r.keys.back().time == doctest::Approx(3.5));
    CHECK(r.keys.back().value == doctest::Approx(5.0));
    checkAgainstDirect("loopOut(\"cycle\")", ctx, r, 0.02, {1, 2, 3});
}

TEST_CASE("expr lower: loopOut pingpong / offset / continue") {
    const std::vector<xe::ExprKey> keys = {key(0, 0), key(1, 10)};
    {
        const xe::ExprContext ctx = context(keys, 4);
        const xe::LowerResult r = lower("loopOut(\"pingpong\")", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactLoop);
        REQUIRE(r.keys.size() == 5);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(r.keys[i].time == doctest::Approx(static_cast<double>(i)));
            CHECK(r.keys[i].value == doctest::Approx(i % 2 == 0 ? 0.0 : 10.0));
        }
    }
    {
        const xe::ExprContext ctx = context(keys, 3);
        const xe::LowerResult r = lower("loopOut(\"offset\")", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        REQUIRE(r.keys.size() == 4);
        CHECK(r.keys[3].value == doctest::Approx(30));
    }
    {
        const xe::ExprContext ctx = context(keys, 3);
        const xe::LowerResult r = lower("loopOut(\"continue\")", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        REQUIRE(r.keys.size() == 3);
        CHECK(r.keys[2].time == doctest::Approx(3));
        CHECK(r.keys[2].value == doctest::Approx(30));
        checkAgainstDirect("loopOut(\"continue\")", ctx, r, 1e-4);
    }
    {
        // pingpong over an eased and a held segment: reversed curves.
        const std::vector<xe::ExprKey> k2 = {key(0, 0, xt::Easing::bezier(0.1f, 0.6f, 0.3f, 1)),
                                             key(1, 10, xt::Easing::hold()), key(2, 20)};
        const xe::ExprContext ctx = context(k2, 8);
        const xe::LowerResult r = lower("loopOut(\"pingpong\")", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactLoop);
        checkKeyTimes(r, 8);
        checkAgainstDirect("loopOut(\"pingpong\")", ctx, r, 0.01, {2, 4, 6});
    }
}

TEST_CASE("expr lower: loopIn") {
    const std::vector<xe::ExprKey> keys = {key(2, 0), key(3, 10), key(4, 0)};
    const xe::ExprContext ctx = context(keys, 4);
    for (const char* text :
         {"loopIn(\"cycle\")", "loopIn(\"pingpong\")", "loopIn(\"offset\")", "loopIn(\"continue\")"}) {
        const xe::LowerResult r = lower(text, ctx);
        INFO(text);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactLoop);
        checkKeyTimes(r, 4);
        checkAgainstDirect(text, ctx, r, 1e-4);
    }
    const xe::LowerResult r = lower("loopIn(\"cycle\")", ctx);
    REQUIRE(r.keys.size() == 5);
    CHECK(r.keys[0].time == doctest::Approx(0));
    CHECK(r.keys[1].value == doctest::Approx(10));
}

TEST_CASE("expr lower: valueAtTime(time - d) shifts the keys") {
    const xt::Easing ease = xt::Easing::bezier(0.25f, 0.1f, 0.25f, 1);
    const std::vector<xe::ExprKey> keys = {key(0, 0, ease), key(1, 10)};
    {
        const xe::ExprContext ctx = context(keys, 2);
        const xe::LowerResult r = lower("valueAtTime(time - 0.5)", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactShift);
        REQUIRE(r.keys.size() == 2);
        CHECK(r.keys[0].time == doctest::Approx(0.5));
        CHECK(r.keys[1].time == doctest::Approx(1.5));
        CHECK(r.keys[0].interp == xe::LoweredInterp::Easing);
        CHECK(r.keys[0].easing.y1 == 0.1f);
    }
    {
        // Staggered by index (58 §58.3), through a variable.
        const xe::ExprContext ctx = context(keys, 2, 3);
        const std::string text = "var d = (index - 1) * 0.25; valueAtTime(time - d)";
        const xe::LowerResult r = lower(text, ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactShift);
        CHECK(r.keys[0].time == doctest::Approx(0.5));
        checkAgainstDirect(text, ctx, r, 1e-4);
    }
    {
        // Shifted left: the eased segment is cut at 0 with the rest of its curve.
        const xe::ExprContext ctx = context(keys, 2);
        const xe::LowerResult r = lower("valueAtTime(time + 0.4)", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactShift);
        REQUIRE(r.keys.size() == 2);
        CHECK(r.keys[0].time == 0);
        CHECK(r.keys[1].time == doctest::Approx(0.6));
        checkAgainstDirect("valueAtTime(time + 0.4)", ctx, r, 0.002);
    }
    {
        // Past the end: cut at the duration.
        const xe::ExprContext ctx = context(keys, 1.2);
        const xe::LowerResult r = lower("valueAtTime(time - 0.5)", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactShift);
        CHECK(r.keys.back().time == doctest::Approx(1.2));
        checkAgainstDirect("valueAtTime(time - 0.5)", ctx, r, 0.002);
    }
}

TEST_CASE("expr lower: linear in time gives two linear keys") {
    const xe::ExprContext ctx = context(kFlat, 2, 2);
    for (const char* text : {"value + time * 90", "time * 2 + index", "var s = 45; value - s * time / 2"}) {
        const xe::LowerResult r = lower(text, ctx);
        INFO(text);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::ExactLinear);
        REQUIRE(r.keys.size() == 2);
        CHECK(r.keys[0].time == 0);
        CHECK(r.keys[1].time == 2);
        checkAgainstDirect(text, ctx, r, 1e-4);  // single-precision playback
    }
    const xe::LowerResult r = lower("value + time * 90", ctx);
    CHECK(r.keys[0].value == doctest::Approx(10));
    CHECK(r.keys[1].value == doctest::Approx(190));
    // value on an animated row is not linear: sampled.
    const std::vector<xe::ExprKey> anim = {key(0, 0, xt::Easing::cssEase()), key(2, 50)};
    CHECK(lower("value + time * 90", context(anim, 2)).method == xe::LowerMethod::Sampled);
}

TEST_CASE("expr lower: sampled expressions stay within the tolerance") {
    struct Case {
        const char* text;
        double tol;
    };
    const Case cases[] = {
        {"Math.sin(time * 3) * 50", 0.25},
        {"wiggle(2, 6)", 0.25},
        {"value + noise(time * 2) * 20", 0.1},
        {"ease(time, 1, 3, 0, 1)", 0.002},
        {"time > 2 ? 1 : 0", 0.002},
        {"var a = 0; for (var i = 1; i <= 3; i++) { a += Math.sin(time * i) / i; } a", 0.002},
        {"linear(time, 0, 4, [0, 0], [100, 50])[1]", 0.25},
    };
    for (const Case& c : cases) {
        xe::ExprContext ctx = context(kFlat, 4, 1, 42);
        ctx.tolerance = c.tol;
        const xe::LowerResult r = lower(c.text, ctx);
        INFO(c.text);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        CHECK(r.method == xe::LowerMethod::Sampled);
        checkKeyTimes(r, 4);
        CHECK(r.keys.size() < 241);
        checkAgainstDirect(c.text, ctx, r, c.tol);
    }
}

TEST_CASE("expr lower: wiggle(2, 6) over 15 s -- key count after simplification") {
    for (const auto& [kind, name] : {std::pair{xe::ExprPropertyKind::Position, "position (0.25)"},
                                     std::pair{xe::ExprPropertyKind::Angle, "angle (0.1)"},
                                     std::pair{xe::ExprPropertyKind::Other, "other (0.002)"}}) {
        xe::ExprContext ctx = context(kFlat, 15, 1, 0xC0FFEEu);
        ctx.tolerance = xe::exprTolerance(kind);
        const xe::LowerResult r = lower("wiggle(2, 6)", ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        MESSAGE("wiggle(2, 6), 15 s, tolerance " << std::string(name) << ": " << r.keys.size()
                                                 << " keys from 901 samples");
        CHECK(r.keys.size() <= 901);
        CHECK(r.keys.front().value == doctest::Approx(r.keys.back().value));  // seamless
        checkAgainstDirect("wiggle(2, 6)", ctx, r, ctx.tolerance);
    }
}

TEST_CASE("expr lower: posterizeTime(12) gives Hold keys") {
    xe::ExprContext ctx = context(kFlat, 3, 1, 5);
    ctx.tolerance = 0.25;
    const xe::LowerResult r = lower("posterizeTime(12); wiggle(2, 6)", ctx);
    REQUIRE(r.status == xe::ExprStatus::Ok);
    CHECK(r.method == xe::LowerMethod::Posterized);
    checkKeyTimes(r, 3);
    CHECK(r.keys.size() <= 37);
    CHECK(r.keys.size() > 10);
    for (const xe::LoweredKey& k : r.keys) {
        CHECK(k.interp == xe::LoweredInterp::Hold);
        const double steps = k.time * 12;
        CHECK(std::fabs(steps - std::round(steps)) < 1e-9);
    }
    // 58 §58.4's example: posterized stagger.
    const std::vector<xe::ExprKey> keys = {key(0, 0), key(1, 1)};
    const xe::ExprContext c2 = context(keys, 2, 3);
    const xe::LowerResult s =
        lower("posterizeTime(12);\nvar d = (index - 1) * 0.08;\nvalueAtTime(time - d)", c2);
    REQUIRE(s.status == xe::ExprStatus::Ok);
    CHECK(s.method == xe::LowerMethod::Posterized);
    checkAgainstDirect("posterizeTime(12);\nvar d = (index - 1) * 0.08;\nvalueAtTime(time - d)", c2, s, 1e-6);
}

TEST_CASE("expr lower: run-time leaves answer NeedsRuntime") {
    const xe::ExprContext ctx = context(kFlat, 2);
    for (const char* text :
         {"port(\"hp\") * 2", "wiggle(2, 6) * port(\"shake\")", "layer(\"Label\").width + 24",
          "randomPerPlay(0, 10)", "port(\"hp\").valueAtTime(time - 0.5)", "time > 5 ? port(\"a\") : 1"}) {
        const xe::LowerResult r = lower(text, ctx);
        INFO(text);
        CHECK(r.status == xe::ExprStatus::NeedsRuntime);
        CHECK(r.keys.empty());
        CHECK(r.diagnostics.empty());
    }
}

TEST_CASE("expr lower: errors give diagnostics and no keys") {
    const xe::ExprContext ctx = context(kFlat, 2);
    for (const char* text : {"1 / 0", "value / (time - 1)", "Math.sqrt(time - 1)", "var x; x",
                             "[1, 2] * [1, 2]", "while (true) {} 1"}) {
        const xe::LowerResult r = lower(text, ctx);
        INFO(text);
        CHECK(r.status == xe::ExprStatus::Error);
        CHECK(r.keys.empty());
        REQUIRE(r.diagnostics.size() == 1);
        CHECK(r.diagnostics.front().code.rfind("expr.", 0) == 0);
    }
    // A program that did not parse, a row without keys, a bad duration.
    const xe::ParseResult bad = xe::parseExpression("1 +");
    CHECK(xe::lowerExpression(bad.program, ctx).status == xe::ExprStatus::Error);
    const std::vector<xe::ExprKey> none;
    const xe::LowerResult empty = lower("value", context(none, 1));
    CHECK(empty.status == xe::ExprStatus::Error);
    CHECK(empty.diagnostics.front().code == "expr.row");
    const std::vector<xe::ExprKey> unordered = {key(1, 0), key(0.5, 1)};
    CHECK(lower("value", context(unordered, 1)).diagnostics.front().code == "expr.row");
    CHECK(lower("value", context(kFlat, -1)).status == xe::ExprStatus::Error);
    CHECK(lower("value", context(kFlat, NAN)).status == xe::ExprStatus::Error);
    // An array result for a component it does not have.
    xe::ExprContext z = context(kFlat, 1);
    z.component = 2;
    CHECK(lower("[1, 2]", z).status == xe::ExprStatus::Error);
    z.component = 1;
    CHECK(lower("[1, 2]", z).keys.front().value == doctest::Approx(2));
}

TEST_CASE("expr lower: a duration of zero gives one key") {
    const xe::LowerResult r = lower("wiggle(2, 6)", context(kFlat, 0));
    REQUIRE(r.status == xe::ExprStatus::Ok);
    CHECK(r.keys.size() == 1);
}

TEST_CASE("expr lower: fuzz -- expression texts never crash") {
    const std::vector<std::string> seeds = {
        "wiggle(2, 6)",
        "loopOut(\"pingpong\")",
        "valueAtTime(time - 0.5)",
        "value + time * 90",
        "posterizeTime(12); var d = (index - 1) * 0.08; valueAtTime(time - d)",
        "var s = 0; for (var i = 0; i < 4; i++) { s += Math.sin(time * i); } s",
        "function f(x) { if (x > 1) { return x * 2; } return -x; } f(time)",
        "[wiggle(1, 2), noise(time)][1] * clamp(time, 0, 1)",
        "seedRandom(3, true); random(5, [1, 2])[0]",
        "ease(time, 0, 2, [0, 1], [10, 20])[1] + linear(time, 5, 1)",
        "port(\"hp\") < 0.2 ? wiggle(8, 4) : value",
        "layer(\"Label\").width + 24",
        "while (time < 1) { break; } loopIn(\"offset\", 1)",
        "x = 1; x++; --x; x %= 2; x",
    };
    const char* const fragments[] = {
        "(",      ")",         "[",       "]",    "{",     "}",     ";",  ",",  "\"", "+",    "*",   "/",
        "%",      "!",         "?",       ":",    "=",     "==",    "&&", "||", ".",  "var ", "if ", "for ",
        "while ", "function ", "return ", "time", "value", "1e400", "-",  "0",  "\n", "Math."};
    const std::vector<xe::ExprKey> keys = {key(0, 0, xt::Easing::cssEaseInOut()),
                                           key(1, 10, xt::Easing::hold()), key(2, 5)};
    const xe::ExprContext ctx = context(keys, 2.5, 2, 99);
    uint32_t rng = 12345;
    auto next = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    size_t ok = 0;
    size_t errors = 0;
    for (const std::string& seed : seeds) {
        for (int round = 0; round < 60; ++round) {
            std::string text = seed;
            const int edits = round == 0 ? 0 : 1 + static_cast<int>(next() % 2);
            for (int e = 0; e < edits && !text.empty(); ++e) {
                const size_t at = next() % (text.size() + 1);
                switch (next() % 4) {
                    case 0:
                        text.insert(at, fragments[next() % std::size(fragments)]);
                        break;
                    case 1:
                        text.erase(std::min(at, text.size() - 1), 1 + next() % 3);
                        break;
                    case 2:
                        text.resize(at);
                        break;
                    default:
                        text[std::min(at, text.size() - 1)] = static_cast<char>(32 + next() % 95);
                        break;
                }
            }
            const xe::ParseResult p = xe::parseExpression(text);
            if (!p.ok()) {
                CHECK_FALSE(p.diagnostics.empty());
                ++errors;
                continue;
            }
            const xe::LowerResult r = xe::lowerExpression(p.program, ctx);
            CHECK((r.status != xe::ExprStatus::Error || !r.diagnostics.empty()));
            if (r.status == xe::ExprStatus::Ok) {
                CHECK_FALSE(r.keys.empty());
                ++ok;
            }
            const xe::SplitResult sp = xe::splitRuntime(p.program, ctx);
            CHECK((sp.status == xe::ExprStatus::Ok || !sp.diagnostics.empty()));
        }
    }
    MESSAGE("expression fuzz: " << ok << " lowered, " << errors << " rejected while parsing");
}

TEST_CASE("expr lower: compile time of 66 expressions over 15 s") {
    const std::vector<std::string> kinds = {
        "wiggle(2, 6)",
        "loopOut(\"cycle\")",
        "valueAtTime(time - (index - 1) * 0.08)",
        "value + time * 90",
        "posterizeTime(12); wiggle(3, 4)",
        "Math.sin(time * 4 + index) * 20",
        "var s = 0; for (var i = 1; i <= 3; i++) { s += noise(time * i) / i; } value + s * 10",
        "ease(time, 0, 2, 0, 100) + wiggle(1, 2)",
        "loopOut(\"pingpong\")",
        "clamp(value + random(-1, 1), 0, 20)",
        "function bob(t) { return Math.abs(Math.sin(t * 3)) * 12; } bob(time)",
    };
    const std::vector<xe::ExprKey> keys = {key(0, 0, xt::Easing::cssEaseInOut()), key(0.5, 10), key(1.2, 3)};
    std::vector<xe::Program> programs;
    for (int i = 0; i < 66; ++i) {
        programs.push_back(parse(kinds[static_cast<size_t>(i) % kinds.size()]));
    }
    size_t totalKeys = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 66; ++i) {
        xe::ExprContext ctx = context(keys, 15, 1 + i % 7, static_cast<uint32_t>(i * 7919));
        ctx.tolerance = xe::kExprTolerance[static_cast<size_t>(i % 3)];
        const xe::LowerResult r = xe::lowerExpression(programs[static_cast<size_t>(i)], ctx);
        REQUIRE(r.status == xe::ExprStatus::Ok);
        totalKeys += r.keys.size();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("66 expressions over 15 s: " << ms << " ms, " << totalKeys << " keys");
}

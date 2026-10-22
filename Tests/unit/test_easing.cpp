#include <cmath>
#include <initializer_list>
#include <vector>

#include "doctest.h"
#include "xt/timeline.h"

using namespace xt;

TEST_CASE("evalEasing: Linear passes p through, Hold always returns 0") {
    Easing linear = Easing::linear();
    CHECK(evalEasing(linear, 0.3f) == doctest::Approx(0.3f));
    CHECK(evalEasing(linear, 0.0f) == doctest::Approx(0.0f));
    CHECK(evalEasing(linear, 1.0f) == doctest::Approx(1.0f));

    Easing hold = Easing::hold();
    CHECK(evalEasing(hold, 0.0f) == doctest::Approx(0.0f));
    CHECK(evalEasing(hold, 0.3f) == doctest::Approx(0.0f));
    CHECK(evalEasing(hold, 1.0f) == doctest::Approx(0.0f));
}

namespace {
/// Docs/tasks/T-06.md tolerance for the CSS keyword reference table: 2e-4.
constexpr float kCssTolerance = 2e-4f;

void checkCssCurve(const Easing& easing, const float (&reference)[5]) {
    const float ps[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        float actual = evalEasing(easing, ps[i]);
        CAPTURE(ps[i]);
        CAPTURE(actual);
        CAPTURE(reference[i]);
        CHECK(std::fabs(actual - reference[i]) <= kCssTolerance);
    }
}
}  // namespace

TEST_CASE("evalEasing: CSS keyword bezier reference values") {
    // Docs/tasks/T-06.md reference table (100-iteration bisection ground truth).
    float ease[5] = {0.0f, 0.4085f, 0.8024f, 0.9605f, 1.0f};
    checkCssCurve(Easing::cssEase(), ease);

    float easeIn[5] = {0.0f, 0.0935f, 0.3154f, 0.6219f, 1.0f};
    checkCssCurve(Easing::cssEaseIn(), easeIn);

    float easeOut[5] = {0.0f, 0.3781f, 0.6846f, 0.9065f, 1.0f};
    checkCssCurve(Easing::cssEaseOut(), easeOut);

    float easeInOut[5] = {0.0f, 0.1292f, 0.5f, 0.8708f, 1.0f};
    checkCssCurve(Easing::cssEaseInOut(), easeInOut);
}

TEST_CASE("evalEasing: Steps jumpEnd and jumpStart") {
    Easing jumpEnd = Easing::steps(4, true);
    CHECK(evalEasing(jumpEnd, 0.1f) == doctest::Approx(0.0f));
    CHECK(evalEasing(jumpEnd, 0.25f) == doctest::Approx(0.25f));
    CHECK(evalEasing(jumpEnd, 0.99f) == doctest::Approx(0.75f));
    CHECK(evalEasing(jumpEnd, 1.0f) == doctest::Approx(1.0f));

    Easing jumpStart = Easing::steps(4, false);
    CHECK(evalEasing(jumpStart, 0.0f) == doctest::Approx(0.0f));
    CHECK(evalEasing(jumpStart, 0.1f) == doctest::Approx(0.25f));
    CHECK(evalEasing(jumpStart, 0.99f) == doctest::Approx(1.0f));
}

TEST_CASE("evalEasing: out-of-range p clamps to [0,1]") {
    Easing linear = Easing::linear();
    CHECK(evalEasing(linear, -1.0f) == doctest::Approx(0.0f));
    CHECK(evalEasing(linear, 2.0f) == doctest::Approx(1.0f));

    Easing steps = Easing::steps(4, true);
    CHECK(evalEasing(steps, -1.0f) == doctest::Approx(evalEasing(steps, 0.0f)));
    CHECK(evalEasing(steps, 2.0f) == doctest::Approx(evalEasing(steps, 1.0f)));
}

// -------------------------------------------------------------------------------------------
// T-335: linear(<stop>, ...) (CSS Easing 1), canonical points and piecewise-linear evaluation.
// -------------------------------------------------------------------------------------------

namespace {

LinearStop stop(float out) { return LinearStop{out, 0, {0, 0}}; }
LinearStop stop(float out, float in) { return LinearStop{out, 1, {in, 0}}; }
LinearStop stop(float out, float in0, float in1) { return LinearStop{out, 2, {in0, in1}}; }

std::vector<EasingPoint> canon(std::initializer_list<LinearStop> stops) {
    std::vector<LinearStop> list(stops);
    std::vector<EasingPoint> out;
    REQUIRE(linearEasingPoints(list, out));
    return out;
}

void checkPoints(const std::vector<EasingPoint>& got, std::initializer_list<EasingPoint> want) {
    REQUIRE(got.size() == want.size());
    size_t i = 0;
    for (const EasingPoint& w : want) {
        CAPTURE(i);
        CHECK(got[i].in == doctest::Approx(w.in));
        CHECK(got[i].out == doctest::Approx(w.out));
        ++i;
    }
}

Easing linearPoints(uint16_t offset, uint16_t count) {
    Easing e;
    e.kind = EasingKind::LinearPoints;
    e.pointOffset = offset;
    e.pointCount = count;
    return e;
}

}  // namespace

TEST_CASE("T-335: linear() canonical points: missing ends, even spread, clamping, two inputs") {
    // Missing first/last inputs are 0 / 1; the middle run is spread evenly.
    checkPoints(canon({stop(0), stop(0.25f), stop(1)}), {{0, 0}, {0.5f, 0.25f}, {1, 1}});
    // A run between two given inputs: 0 .. 0.8 over three intervals.
    checkPoints(canon({stop(0), stop(0.2f), stop(0.4f), stop(1, 0.8f), stop(1)}),
                {{0, 0}, {0.8f / 3, 0.2f}, {1.6f / 3, 0.4f}, {0.8f, 1}, {1, 1}});
    // An input below an earlier one is raised to it.
    checkPoints(canon({stop(0), stop(0.3f, 0.6f), stop(0.6f, 0.4f), stop(1)}),
                {{0, 0}, {0.6f, 0.3f}, {0.6f, 0.6f}, {1, 1}});
    // Two inputs on one stop are two points.
    checkPoints(canon({stop(0), stop(0.5f, 0.25f, 0.75f), stop(1)}),
                {{0, 0}, {0.25f, 0.5f}, {0.75f, 0.5f}, {1, 1}});
    // Fewer than two stops is invalid.
    std::vector<LinearStop> one{stop(0)};
    std::vector<EasingPoint> out;
    CHECK_FALSE(linearEasingPoints(one, out));
    CHECK(out.empty());
}

TEST_CASE("T-335: evalEasing LinearPoints is piecewise linear (hand-computed)") {
    // linear(0, 0.8 20%, 1) at pool offset 1 (pool[0] belongs to another easing).
    std::vector<EasingPoint> pool{{9, 9}};
    std::vector<LinearStop> stops{stop(0), stop(0.8f, 0.2f), stop(1)};
    REQUIRE(linearEasingPoints(stops, pool));
    const Easing e = linearPoints(1, 3);
    CHECK(evalEasing(e, 0.0f, pool) == doctest::Approx(0.0f));
    CHECK(evalEasing(e, 0.1f, pool) == doctest::Approx(0.4f));
    CHECK(evalEasing(e, 0.2f, pool) == doctest::Approx(0.8f));
    CHECK(evalEasing(e, 0.6f, pool) == doctest::Approx(0.9f));  // 0.8 + 0.4 / 0.8 * 0.2
    CHECK(evalEasing(e, 1.0f, pool) == doctest::Approx(1.0f));
    CHECK(evalEasing(e, 1.5f, pool) == doctest::Approx(1.0f));  // p clamps to [0,1].

    // A hold (two inputs): linear(0, 0.5 25% 75%, 1).
    std::vector<EasingPoint> hold;
    std::vector<LinearStop> holdStops{stop(0), stop(0.5f, 0.25f, 0.75f), stop(1)};
    REQUIRE(linearEasingPoints(holdStops, hold));
    const Easing h = linearPoints(0, 4);
    CHECK(evalEasing(h, 0.125f, hold) == doctest::Approx(0.25f));
    CHECK(evalEasing(h, 0.5f, hold) == doctest::Approx(0.5f));
    CHECK(evalEasing(h, 0.875f, hold) == doctest::Approx(0.75f));

    // Coincident inputs: the later point wins from there on.
    std::vector<EasingPoint> jump;
    std::vector<LinearStop> jumpStops{stop(0), stop(0.2f, 0.5f), stop(0.9f, 0.5f), stop(1)};
    REQUIRE(linearEasingPoints(jumpStops, jump));
    const Easing j = linearPoints(0, 4);
    CHECK(evalEasing(j, 0.25f, jump) == doctest::Approx(0.1f));
    CHECK(evalEasing(j, 0.5f, jump) == doctest::Approx(0.9f));
    CHECK(evalEasing(j, 0.75f, jump) == doctest::Approx(0.95f));

    // Without the pool (or out of its range) it is plain linear.
    CHECK(evalEasing(e, 0.1f) == doctest::Approx(0.1f));
    CHECK(evalEasing(linearPoints(3, 3), 0.1f, pool) == doctest::Approx(0.1f));
}

TEST_CASE("T-335: steps() and cubic-bezier() ignore the easing point pool") {
    std::vector<EasingPoint> pool{{0, 1}, {1, 0}};
    CHECK(evalEasing(Easing::steps(4, true), 0.3f, pool) == doctest::Approx(0.25f));
    CHECK(evalEasing(Easing::steps(4, false), 0.3f, pool) == doctest::Approx(0.5f));
    CHECK(evalEasing(Easing::cssEase(), 0.5f, pool) == doctest::Approx(evalEasing(Easing::cssEase(), 0.5f)));
    CHECK(evalEasing(Easing::linear(), 0.3f, pool) == doctest::Approx(0.3f));
}

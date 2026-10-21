#include <cmath>
#include <vector>

#include "doctest.h"
#include "sw_paint.h"
#include "xt/command_list.h"
#include "xt/types.h"

/// PaintEval tests. See docs/tasks/T-12.md "測試" / "test_sw_paint.cpp", docs/tasks/T-128.md
/// step 2 (the 256-entry ramp and the incremental span evaluator).

using namespace xt;
using namespace xt::sw;

namespace {

PaintRef makeGradient(PaintKind kind, Vec2 p0, Vec2 p1, float alpha = 1.0f) {
    PaintRef p{};
    p.kind = kind;
    p.color = Color{1, 1, 1, alpha};
    p.p0 = p0;
    p.p1 = p1;
    p.stopOffset = 0;
    p.stopCount = 2;
    return p;
}

}  // namespace

TEST_CASE("PaintEval: linear gradient, horizontal black->white, t at pixel center") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{0, 0}, Vec2{10, 0});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    // Pixel column x=5 -> center (5.5, 0.5) -> t = 5.5/10 = 0.55.
    Color c = eval.at(5, 0);
    CHECK(c.r == doctest::Approx(0.55f).epsilon(0.01));
    CHECK(c.g == doctest::Approx(0.55f).epsilon(0.01));
    CHECK(c.b == doctest::Approx(0.55f).epsilon(0.01));
    CHECK(c.a == doctest::Approx(1.0f));
}

TEST_CASE("PaintEval: radial gradient distance ratio") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Radial, Vec2{10, 10}, Vec2{20, 10});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    // Pixel center (15.5, 10.5) is distance sqrt(5.5^2+0.5^2) from (10,10); radius 10.
    Color center = eval.at(15, 10);
    float dist = std::sqrt(5.5f * 5.5f + 0.5f * 0.5f);
    float expectedT = dist / 10.0f;
    CHECK(center.r == doctest::Approx(expectedT).epsilon(0.01));

    // Far outside the radius clamps to the last stop.
    Color far = eval.at(1000, 10);
    CHECK(far.r == doctest::Approx(1.0f));
}

TEST_CASE("PaintEval: pad behavior clamps t before first / after last stop") {
    std::vector<GradientStop> stops = {
        GradientStop{0.25f, Color{0, 0, 0, 1}},
        GradientStop{0.75f, Color{1, 1, 1, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{0, 0}, Vec2{100, 0});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    // t = 0.5/100 ~ 0.005, before first stop offset 0.25 -> first color.
    Color before = eval.at(0, 0);
    CHECK(before.r == doctest::Approx(0.0f));

    // t = 99.5/100 = 0.995, after last stop offset 0.75 -> last color.
    Color after = eval.at(99, 0);
    CHECK(after.r == doctest::Approx(1.0f));
}

TEST_CASE("PaintEval: p0 == p1 returns the last stop color") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{0.2f, 0.4f, 0.6f, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{5, 5}, Vec2{5, 5});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    Color c = eval.at(50, 50);
    CHECK(c.r == doctest::Approx(0.2f));
    CHECK(c.g == doctest::Approx(0.4f));
    CHECK(c.b == doctest::Approx(0.6f));
}

TEST_CASE("PaintEval: PaintRef::color.a multiplies the sampled stop alpha") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{1, 1, 1, 0.5f}},
        GradientStop{1.0f, Color{1, 1, 1, 0.5f}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{0, 0}, Vec2{10, 0}, 0.5f);
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    Color c = eval.at(5, 0);
    CHECK(c.a == doctest::Approx(0.25f));
}

TEST_CASE("PaintEval: solid paint ignores stops and transform") {
    PaintRef paint{};
    paint.kind = PaintKind::Solid;
    paint.color = Color{0.1f, 0.2f, 0.3f, 0.4f};
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, {}, Mat2x3{}, luts);
    Color c = eval.at(123, 456);
    CHECK(c.r == doctest::Approx(0.1f));
    CHECK(c.g == doctest::Approx(0.2f));
    CHECK(c.b == doctest::Approx(0.3f));
    CHECK(c.a == doctest::Approx(0.4f));
}

// --- T-128: the ramp cache and the incremental span evaluator -----------------------------------

TEST_CASE("GradientLutCache: same stops and alpha share one ramp, different alpha do not") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    std::vector<GradientStop> copy = stops;
    GradientLutCache luts;
    const float* a = luts.get(stops, 1.0f);
    CHECK(luts.get(copy, 1.0f) == a);   // keyed by contents, not by the span's address
    CHECK(luts.get(stops, 0.5f) != a);  // the paint's alpha is part of the key
    luts.reset();
    // The ramp is premultiplied by the paint's alpha (docs/tasks/T-128.md step 2).
    const float* half = luts.get(stops, 0.5f);
    CHECK(half[(kGradientLutSize - 1) * 4 + 3] == doctest::Approx(0.5f));
    CHECK(half[(kGradientLutSize - 1) * 4 + 0] == doctest::Approx(0.5f));
}

TEST_CASE("PaintEval::spanPremul matches at() within the ramp's quantization") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{0, 0}, Vec2{64, 0});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    constexpr int kN = 64;
    std::vector<float> span(static_cast<size_t>(kN) * 4, 0.0f);
    PaintEval::SpanInfo info = eval.spanPremul(0, 0, kN, 1.0f, span.data());
    CHECK(info.first == 0);
    CHECK(info.last == kN);
    CHECK(info.allOpaque);  // every stop is opaque, so source-over is a store
    for (int i = 0; i < kN; ++i) {
        Color point = eval.at(static_cast<float>(i), 0.0f);
        // spanPremul writes premultiplied pixels; `at` is non-premultiplied.
        CHECK(span[static_cast<size_t>(i) * 4 + 3] == doctest::Approx(point.a));
        CHECK(span[static_cast<size_t>(i) * 4 + 0] == doctest::Approx(point.r * point.a).epsilon(0.005));
    }
}

TEST_CASE("PaintEval::spanPremul narrows a span to the visible part of the gradient") {
    // A radial wash that fades to fully transparent: outside its radius every pixel clamps to the
    // transparent end, and the span evaluator must report that range as empty rather than blend it
    // (docs/tasks/T-128.md step 2).
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{1, 1, 1, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 0}},
    };
    PaintRef paint = makeGradient(PaintKind::Radial, Vec2{20, 0}, Vec2{30, 0});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    constexpr int kN = 200;
    std::vector<float> span(static_cast<size_t>(kN) * 4, -1.0f);
    PaintEval::SpanInfo info = eval.spanPremul(0, 0, kN, 1.0f, span.data());
    CHECK(info.first >= 9);   // radius 10 around x = 20: nothing visible before x = 10
    CHECK(info.first <= 11);  // ... and the slack never drops a visible pixel
    CHECK(info.last >= 29);
    CHECK(info.last <= 31);
    CHECK_FALSE(info.allOpaque);
    for (int i = info.first; i < info.last; ++i) {
        CHECK(span[static_cast<size_t>(i) * 4 + 3] >= 0.0f);  // the reported range really was written
    }

    // A span that never reaches the gradient at all is reported as completely empty.
    PaintEval::SpanInfo none = eval.spanPremul(500, 0, kN, 1.0f, span.data());
    CHECK(none.first >= none.last);
}

TEST_CASE("PaintEval::spanPremul scales by the span's coverage") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{1, 1, 1, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    PaintRef paint = makeGradient(PaintKind::Linear, Vec2{0, 0}, Vec2{8, 0});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);

    constexpr int kN = 8;
    std::vector<float> span(static_cast<size_t>(kN) * 4, 0.0f);
    PaintEval::SpanInfo info = eval.spanPremul(0, 0, kN, 0.5f, span.data());
    CHECK_FALSE(info.allOpaque);  // half coverage is not a store
    CHECK(span[3] == doctest::Approx(0.5f));
    CHECK(span[0] == doctest::Approx(0.5f));
}

TEST_CASE("PaintEval: conic gradient, the clockwise angle from the start ray (T-201)") {
    std::vector<GradientStop> stops = {
        GradientStop{0.0f, Color{0, 0, 0, 1}},
        GradientStop{1.0f, Color{1, 1, 1, 1}},
    };
    // Centre (50, 50), start ray straight up (0deg).
    PaintRef paint = makeGradient(PaintKind::Conic, Vec2{50, 50}, Vec2{50, 49});
    GradientLutCache luts;
    PaintEval eval = PaintEval::make(paint, stops, Mat2x3{}, luts);
    // Right of the centre = a quarter turn, below = half, left = three quarters.
    CHECK(eval.at(80, 49).r == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(eval.at(49, 80).r == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(eval.at(20, 49).r == doctest::Approx(0.75f).epsilon(0.02));
    // The span evaluator agrees with the per-pixel one.
    std::vector<float> span(4 * 100);
    eval.spanPremul(0, 20, 100, 1.0f, span.data());
    CHECK(span[4 * 80] == doctest::Approx(eval.at(80, 20).r).epsilon(0.01));
    CHECK(span[4 * 20] == doctest::Approx(eval.at(20, 20).r).epsilon(0.01));
}

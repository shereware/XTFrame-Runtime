#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "doctest.h"
#include "perf_clock.h"
#include "sw_effect_math.h"
#include "sw_layer.h"
#include "xt/types.h"

/// sw_effect_math tests: box-approximated Gaussian blur, alpha dilation, 8SSEDT signed distance,
/// and the ColorAdjust matrix. See docs/design/18-effects.md §18.3/§18.6, docs/tasks/T-53a.md.

using namespace xt;
using namespace xt::sw;
using namespace xt::sw::fx;

namespace {

Surface makeSurface(int w, int h, const Color& fill = Color{0, 0, 0, 0}) {
    Surface s;
    s.w = w;
    s.h = h;
    s.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            s.set(x, y, fill);
        }
    }
    return s;
}

float sumAlpha(const std::vector<float>& a) {
    float total = 0.0f;
    for (float v : a) {
        total += v;
    }
    return total;
}

}  // namespace

TEST_CASE("gaussianBlur: sigma = 0 is a pixel-exact copy") {
    Surface src = makeSurface(8, 8);
    src.set(3, 4, Color{0.2f, 0.4f, 0.6f, 0.8f});
    Surface dst, scratch;
    gaussianBlur(src, dst, scratch, 0.0f, 0.0f, /*premultiply=*/false);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            Color s = src.get(x, y);
            Color d = dst.get(x, y);
            CHECK(d.r == doctest::Approx(s.r));
            CHECK(d.g == doctest::Approx(s.g));
            CHECK(d.b == doctest::Approx(s.b));
            CHECK(d.a == doctest::Approx(s.a));
        }
    }
}

TEST_CASE("gaussianBlurAlpha: sigma = 2 bright point is symmetric, conserves mass, lowers the peak") {
    const int w = 32;
    const int h = 32;
    Surface src = makeSurface(w, h);
    src.set(16, 16, Color{1, 1, 1, 1});

    std::vector<float> outAlpha, scratch;
    gaussianBlurAlpha(src, outAlpha, scratch, 2.0f);

    float centerBefore = src.get(16, 16).a;
    float centerAfter = outAlpha[static_cast<size_t>(16) * w + 16];
    CHECK(centerAfter < centerBefore);

    // Symmetry around the center pixel.
    for (int d = 1; d <= 4; ++d) {
        float left = outAlpha[static_cast<size_t>(16) * static_cast<size_t>(w) + static_cast<size_t>(16 - d)];
        float right =
            outAlpha[static_cast<size_t>(16) * static_cast<size_t>(w) + static_cast<size_t>(16 + d)];
        float up = outAlpha[static_cast<size_t>(16 - d) * w + 16];
        float down = outAlpha[static_cast<size_t>(16 + d) * w + 16];
        CHECK(left == doctest::Approx(right).epsilon(0.02));
        CHECK(up == doctest::Approx(down).epsilon(0.02));
        CHECK(left == doctest::Approx(up).epsilon(0.02));
    }

    // Mass conservation within 1%: total alpha before == total alpha after (point mass = 1.0).
    CHECK(sumAlpha(outAlpha) == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("gaussianBlur: premultiplied mode does not darken a translucent edge") {
    const int w = 16;
    const int h = 16;
    Surface src = makeSurface(w, h, Color{0, 0, 0, 0});
    // A half-transparent red disc-ish blob so blurring mixes edge alpha=0 with interior alpha=0.5.
    for (int y = 5; y <= 10; ++y) {
        for (int x = 5; x <= 10; ++x) {
            src.set(x, y, Color{1, 0, 0, 0.5f});
        }
    }

    Surface dst, scratch;
    gaussianBlur(src, dst, scratch, 2.0f, 2.0f, /*premultiply=*/true);

    // A sample point still inside the (shrunk-by-blur) coverage: RGB must not have darkened toward
    // black just because alpha dropped near the edge.
    Color c = dst.get(7, 7);
    CHECK(c.r >= 0.9f);
}

TEST_CASE("dilateAlpha: r = 3 turns a 1px point into a 7x7 square") {
    const int w = 20;
    const int h = 20;
    std::vector<float> in(static_cast<size_t>(w) * h, 0.0f);
    in[static_cast<size_t>(10) * w + 10] = 1.0f;

    std::vector<float> out, scratch;
    dilateAlpha(in, out, scratch, w, h, 3);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bool inSquare = x >= 7 && x <= 13 && y >= 7 && y <= 13;
            float v = out[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            CHECK(v == doctest::Approx(inSquare ? 1.0f : 0.0f));
        }
    }
}

TEST_CASE("dilateAlpha: r = 0 leaves the input unchanged") {
    const int w = 5;
    const int h = 5;
    std::vector<float> in = {0, 0.2f, 0, 0, 0, 0, 0,    0.7f, 0, 0, 0, 0, 0,
                             0, 0,    0, 0, 0, 0, 0.1f, 0,    0, 0, 0, 0};
    std::vector<float> out, scratch;
    dilateAlpha(in, out, scratch, w, h, 0);

    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i] == doctest::Approx(in[i]));
    }
}

TEST_CASE("signedDistance: 10x10 square in a 32x32 field") {
    const int w = 32;
    const int h = 32;
    std::vector<float> alpha(static_cast<size_t>(w) * h, 0.0f);
    // Foreground square: x,y in [11, 20] (10x10), centered at (15.5, 15.5).
    for (int y = 11; y <= 20; ++y) {
        for (int x = 11; x <= 20; ++x) {
            alpha[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = 1.0f;
        }
    }

    std::vector<float> sdf;
    signedDistance(alpha, sdf, w, h);

    // Center of the square: distance to nearest edge is 5 px (half the square's width).
    float center = sdf[static_cast<size_t>(15) * w + 15];
    CHECK(center == doctest::Approx(-5.0f).epsilon(0.15));

    // 3 px outside the right edge (edge at x=20, so x=23 is 3 px beyond it).
    float outside = sdf[static_cast<size_t>(15) * w + 23];
    CHECK(outside == doctest::Approx(3.0f).epsilon(0.25));
}

TEST_CASE("colorAdjustMatrix: brightness adds to every channel, clamped to 1") {
    ColorMatrix m = colorAdjustMatrix(0.2f, 0.0f, 0.0f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.5f, 0.9f, 0.0f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    Color c = s.get(0, 0);
    CHECK(c.r == doctest::Approx(0.7f).epsilon(0.02));
    CHECK(c.g == doctest::Approx(1.0f).epsilon(0.02));  // 0.9 + 0.2 clamps to 1.
    CHECK(c.b == doctest::Approx(0.2f).epsilon(0.02));
}

TEST_CASE("colorAdjustMatrix: contrast = -1 flattens everything to 0.5") {
    ColorMatrix m = colorAdjustMatrix(0.0f, -1.0f, 0.0f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.1f, 0.9f, 0.4f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    Color c = s.get(0, 0);
    CHECK(c.r == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(c.g == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(c.b == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("colorAdjustMatrix: saturation = -1 desaturates to luma") {
    ColorMatrix m = colorAdjustMatrix(0.0f, 0.0f, -1.0f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.8f, 0.3f, 0.1f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    Color c = s.get(0, 0);
    float luma = 0.2126f * 0.8f + 0.7152f * 0.3f + 0.0722f * 0.1f;
    CHECK(c.r == doctest::Approx(luma).epsilon(0.02));
    CHECK(c.g == doctest::Approx(luma).epsilon(0.02));
    CHECK(c.b == doctest::Approx(luma).epsilon(0.02));
}

TEST_CASE("colorAdjustMatrix: hue rotate 180deg turns red into cyan") {
    ColorMatrix m = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 180.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{1.0f, 0.0f, 0.0f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    Color c = s.get(0, 0);
    CHECK(c.r == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(c.g == doctest::Approx(0.6f).epsilon(0.05));
    CHECK(c.b == doctest::Approx(0.6f).epsilon(0.05));
}

TEST_CASE("colorAdjustMatrix: T-334 invert 0 / 0.5 / 1 on a fixed colour") {
    // The Surface stores quantized channels, so the checks allow about 1/255.
    const Color in{0.2f, 0.6f, 0.9f, 1.0f};
    const float amounts[] = {0.0f, 0.5f, 1.0f};
    for (float a : amounts) {
        CAPTURE(a);
        ColorMatrix m = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 0.0f, a, 0.0f);
        Surface s = makeSurface(1, 1);
        s.set(0, 0, in);
        applyColorMatrix(s, m, 1.0f);
        Color c = s.get(0, 0);
        // CSS Filter Effects: c' = c + a * (1 - 2c).
        CHECK(c.r == doctest::Approx(in.r + a * (1.0f - 2.0f * in.r)).epsilon(0.004));
        CHECK(c.g == doctest::Approx(in.g + a * (1.0f - 2.0f * in.g)).epsilon(0.004));
        CHECK(c.b == doctest::Approx(in.b + a * (1.0f - 2.0f * in.b)).epsilon(0.004));
    }
    // invert(0.5) is mid grey for every input.
    ColorMatrix half = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, in);
    applyColorMatrix(s, half, 1.0f);
    CHECK(s.get(0, 0).r == doctest::Approx(0.5f));
    CHECK(s.get(0, 0).b == doctest::Approx(0.5f));
}

TEST_CASE("colorAdjustMatrix: T-334 sepia 1 on a fixed colour") {
    ColorMatrix m = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.2f, 0.4f, 0.6f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    Color c = s.get(0, 0);
    CHECK(c.r == doctest::Approx(0.393f * 0.2f + 0.769f * 0.4f + 0.189f * 0.6f).epsilon(0.004));  // 0.5005
    CHECK(c.g == doctest::Approx(0.349f * 0.2f + 0.686f * 0.4f + 0.168f * 0.6f).epsilon(0.004));  // 0.4450
    CHECK(c.b == doctest::Approx(0.272f * 0.2f + 0.534f * 0.4f + 0.131f * 0.6f).epsilon(0.004));  // 0.3466
    // A saturated input overflows and is clamped to 1 (18.3: the result is clamped 0..1).
    s.set(0, 0, Color{1.0f, 1.0f, 1.0f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    CHECK(s.get(0, 0).r == doctest::Approx(1.0f));
    CHECK(s.get(0, 0).b == doctest::Approx(0.937f).epsilon(0.004));
}

TEST_CASE("colorAdjustMatrix: T-334 invert is applied after hue and before sepia") {
    // brightness 0.2 then invert 1: 1 - (0.3 + 0.2) = 0.5, not (1 - 0.3) + 0.2 = 0.9.
    ColorMatrix m = colorAdjustMatrix(0.2f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.3f, 0.3f, 0.3f, 1.0f});
    applyColorMatrix(s, m, 1.0f);
    CHECK(s.get(0, 0).r == doctest::Approx(0.5f).epsilon(0.004));
    // invert 1 then sepia 1 on black: sepia(white) = (1, 1, 0.937) clamped.
    ColorMatrix both = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    s.set(0, 0, Color{0.0f, 0.0f, 0.0f, 1.0f});
    applyColorMatrix(s, both, 1.0f);
    CHECK(s.get(0, 0).b == doctest::Approx(0.937f).epsilon(0.004));
}

TEST_CASE("applyColorMatrix: opacity halves alpha") {
    ColorMatrix m = colorAdjustMatrix(0.0f, 0.0f, 0.0f, 0.0f);
    Surface s = makeSurface(1, 1);
    s.set(0, 0, Color{0.3f, 0.4f, 0.5f, 0.8f});
    applyColorMatrix(s, m, 0.5f);
    Color c = s.get(0, 0);
    CHECK(c.a == doctest::Approx(0.4f));
    // A Surface is premultiplied since T-102, so halving alpha halves every channel with it; the
    // colour the viewer sees (channel / alpha) is what the identity matrix leaves unchanged.
    CHECK(c.r == doctest::Approx(0.15f).epsilon(0.02));
    CHECK(c.g == doctest::Approx(0.2f).epsilon(0.02));
    CHECK(c.b == doctest::Approx(0.25f).epsilon(0.02));
}

TEST_CASE("perf: 512x512 gaussianBlur sigma = 8, three passes") {
    const int w = 512;
    const int h = 512;
    Surface src = makeSurface(w, h, Color{0.5f, 0.5f, 0.5f, 1.0f});
    Surface dst, scratch;

    // A couple of untimed warm-up runs, then time one representative run.
    for (int i = 0; i < 2; ++i) {
        gaussianBlur(src, dst, scratch, 8.0f, 8.0f, /*premultiply=*/true);
    }
    double t0 = xt::perf::nowSeconds();
    gaussianBlur(src, dst, scratch, 8.0f, 8.0f, /*premultiply=*/true);
    double elapsedMs = (xt::perf::nowSeconds() - t0) * 1000.0;

    char line[128];
    std::snprintf(line, sizeof(line), "512x512 gaussianBlur sigma=8: %.3f ms", elapsedMs);
    MESSAGE(line);
    // T-267a: wall clock, so Release only and at 10x the original 50 ms bound.
#if defined(NDEBUG)
    CHECK(elapsedMs < 500.0);
#endif
}

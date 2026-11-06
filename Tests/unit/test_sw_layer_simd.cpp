#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "sw_blend.h"
#include "sw_layer.h"
#include "sw_simd_kernels.h"
#include "xt/simd.h"

/// T-643: the layer compositing kernels (`SwSimdOps::spanLayerBlendF` / `spanLayerBlendU8`) against
/// the per-pixel `blendPixelPremul` loop LayerStack::popLayer ran before them, byte for byte, for
/// every blend mode and every table this machine can run; and the pool surfaces that are no longer
/// cleared on acquire (only the rows a layer draws into are), checked against a cleared layer.

using namespace xt;
using namespace xt::sw;

namespace {

/// Deterministic xorshift (same generator as test_simd.cpp).
struct Rng {
    uint32_t s = 0x2545F491u;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
};

/// Alpha values the blend arithmetic treats specially: 0 (skipped / divided out), denormal-small
/// and tiny (huge un-premultiplied colours), 1/255, 1/2 and 1, plus random ones.
float edgeAlpha(Rng& rng) {
    static const float kEdges[] = {0.0f, 1e-38f, 1e-30f, 1e-7f, 1.0f / 255.0f, 0.5f, 1.0f};
    const uint32_t pick = rng.next() % 12u;
    return pick < 7u ? kEdges[pick] : rng.unit();
}

/// One premultiplied colour channel for alpha `a`: usually `<= a` (a valid premultiplied pixel),
/// sometimes a coarse fraction of `a` (equal channels: the non-separable modes' ties), sometimes
/// above `a` (out-of-range colours, which drive clipColor and the dodge/burn limits). Only for an
/// alpha of at least 1/255: a channel far above a tiny alpha un-premultiplies past the float range,
/// where the non-separable modes' sort meets infinities and NaN -- no valid premultiplied pixel
/// gets there, and the identity is not claimed for it.
float channel(Rng& rng, float a) {
    switch (rng.next() % 4u) {
        case 0:
            return static_cast<float>(rng.next() % 5u) * 0.25f * a;
        case 1:
            return a >= 1.0f / 255.0f ? rng.unit() : rng.unit() * a;
        default:
            return rng.unit() * a;
    }
}

std::vector<float> randomPixels(Rng& rng, int n) {
    std::vector<float> px(static_cast<size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        const float a = edgeAlpha(rng);
        for (int c = 0; c < 3; ++c) {
            px[static_cast<size_t>(i) * 4 + static_cast<size_t>(c)] = channel(rng, a);
        }
        px[static_cast<size_t>(i) * 4 + 3] = a;
    }
    return px;
}

std::vector<uint8_t> randomBytes(Rng& rng, int n) {
    std::vector<uint8_t> px(static_cast<size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) {
            px[static_cast<size_t>(i) * 4 + static_cast<size_t>(c)] =
                static_cast<uint8_t>(rng.next() & 0xFFu);
        }
        const uint32_t pick = rng.next() % 4u;
        if (pick == 0) {
            px[static_cast<size_t>(i) * 4 + 3] = 0;  // transparent, colour bytes left non-zero
        } else if (pick == 1) {
            px[static_cast<size_t>(i) * 4 + 3] =
                static_cast<uint8_t>(pick == 1 && (rng.next() & 1u) ? 255 : 1);
        }
    }
    return px;
}

/// popLayer's per-pixel loop before T-643, on a float row.
void referenceF(float* dst, const float* src, int n, float opacity, BlendMode mode) {
    for (int i = 0; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        const Color s{sp[0] * opacity, sp[1] * opacity, sp[2] * opacity, sp[3] * opacity};
        if (s.a <= 0.0f) {
            continue;
        }
        float* p = dst + static_cast<size_t>(i) * 4;
        const Color r = blendPixelPremul(mode, s, Color{p[0], p[1], p[2], p[3]});
        p[0] = r.r;
        p[1] = r.g;
        p[2] = r.b;
        p[3] = r.a;
    }
}

/// popLayer's per-pixel loop before T-643, on an RGBA8 row (loadRow / storeRow).
void referenceU8(uint8_t* dst, const float* src, int n, float opacity, BlendMode mode, bool premul) {
    for (int i = 0; i < n; ++i) {
        const float* sp = src + static_cast<size_t>(i) * 4;
        const Color s{sp[0] * opacity, sp[1] * opacity, sp[2] * opacity, sp[3] * opacity};
        if (s.a <= 0.0f) {
            continue;
        }
        uint8_t* p = dst + static_cast<size_t>(i) * 4;
        storePixelU8(p, blendPixelPremul(mode, s, loadPixelU8(p, premul)), premul);
    }
}

const char* modeName(int m) {
    static const char* kNames[] = {"Normal",     "Multiply",   "Screen",    "Overlay",    "Darken",
                                   "Lighten",    "ColorDodge", "ColorBurn", "HardLight",  "SoftLight",
                                   "Difference", "Exclusion",  "Hue",       "Saturation", "Color",
                                   "Luminosity", "Add"};
    return kNames[m];
}

/// Runs one table over every mode and opacity, `rounds` random rows each.
void checkTable(const SwSimdOps& ops, int rounds) {
    Rng rng;
    const float opacities[] = {1.0f, 0.73f, 0.5f, 1.0f / 255.0f, 0.0f};
    const int n = 103;  // not a multiple of 4: the scalar tail runs too
    for (int m = 0; m <= static_cast<int>(BlendMode::Add); ++m) {
        const BlendMode mode = static_cast<BlendMode>(m);
        for (float opacity : opacities) {
            for (int round = 0; round < rounds; ++round) {
                const std::vector<float> src = randomPixels(rng, n);
                std::vector<float> a = randomPixels(rng, n);
                std::vector<float> b = a;
                ops.spanLayerBlendF(a.data(), src.data(), n, opacity, mode);
                referenceF(b.data(), src.data(), n, opacity, mode);
                const std::string table = ops.name;
                const std::string name = modeName(m);
                CAPTURE(table);
                CAPTURE(name);
                CAPTURE(opacity);
                std::string firstDiff;
                for (size_t i = 0; i < a.size() && firstDiff.empty(); ++i) {
                    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) {
                        const size_t px = i / 4 * 4;
                        firstDiff = "pixel " + std::to_string(i / 4) + " channel " + std::to_string(i % 4) +
                                    ": kernel " + std::to_string(a[i]) + " reference " +
                                    std::to_string(b[i]) + " src " + std::to_string(src[px]) + "," +
                                    std::to_string(src[px + 1]) + "," + std::to_string(src[px + 2]) + "," +
                                    std::to_string(src[px + 3]);
                    }
                }
                CAPTURE(firstDiff);
                CHECK(firstDiff.empty());
                for (int premul = 0; premul < 2; ++premul) {
                    std::vector<uint8_t> ua = randomBytes(rng, n);
                    std::vector<uint8_t> ub = ua;
                    ops.spanLayerBlendU8(ua.data(), src.data(), n, opacity, mode, premul != 0);
                    referenceU8(ub.data(), src.data(), n, opacity, mode, premul != 0);
                    CAPTURE(premul);
                    CHECK(ua == ub);
                }
            }
        }
    }
}

}  // namespace

TEST_CASE(
    "sw layer blend: every mode's kernel matches the scalar blendPixelPremul loop byte for byte (T-643)") {
    // The dispatched table (AVX2 or SSE2 here), the scalar table (XT_SW_FORCE_SCALAR) and, when the
    // CPU has it, the AVX2 table explicitly, so an SSE2-only dispatch still checks it.
    static const SwSimdOps scalar = simdk::makeOps<simd::f32x4s>("scalar");
    checkTable(swSimd(), 40);
    checkTable(scalar, 10);
#if XT_SIMD_SSE2
    static const SwSimdOps sse2 = simdk::makeOps<simd::f32x4>("sse2");
    checkTable(sse2, 10);
    if (swCpuHasAvx2()) {
        checkTable(swSimdAvx2Ops(), 10);
    }
#endif
}

TEST_CASE("sw layer blend: the f32x4 comparison, mask, sqrt, abs and transpose ops match f32x4s (T-643)") {
    Rng rng;
    for (int round = 0; round < 200; ++round) {
        float a[4];
        float b[4];
        for (int i = 0; i < 4; ++i) {
            a[i] = (rng.unit() - 0.25f) * 4.0f;
            b[i] = (rng.next() & 3u) == 0 ? a[i] : (rng.unit() - 0.25f) * 4.0f;
        }
        const simd::f32x4 va = simdk::vload<simd::f32x4>(a);
        const simd::f32x4 vb = simdk::vload<simd::f32x4>(b);
        const simd::f32x4s sa = simdk::vload<simd::f32x4s>(a);
        const simd::f32x4s sb = simdk::vload<simd::f32x4s>(b);
        const auto same = [](simd::f32x4 v, simd::f32x4s s) {
            float x[4];
            float y[4];
            simd::store(x, v);
            simd::store(y, s);
            return std::memcmp(x, y, sizeof(x)) == 0;
        };
        CHECK(same(simd::cmpLt(va, vb), simd::cmpLt(sa, sb)));
        CHECK(same(simd::cmpLe(va, vb), simd::cmpLe(sa, sb)));
        CHECK(same(simd::cmpEq(va, vb), simd::cmpEq(sa, sb)));
        CHECK(same(simd::maskAnd(simd::cmpLt(va, vb), simd::cmpLe(vb, va)),
                   simd::maskAnd(simd::cmpLt(sa, sb), simd::cmpLe(sb, sa))));
        CHECK(same(simd::maskOr(simd::cmpLt(va, vb), simd::cmpEq(va, vb)),
                   simd::maskOr(simd::cmpLt(sa, sb), simd::cmpEq(sa, sb))));
        CHECK(same(simd::maskAndNot(simd::cmpLe(va, vb), simd::cmpEq(va, vb)),
                   simd::maskAndNot(simd::cmpLe(sa, sb), simd::cmpEq(sa, sb))));
        CHECK(same(simd::sqrt(simd::abs(va)), simd::sqrt(simd::abs(sa))));
        CHECK(same(simd::abs(va), simd::abs(sa)));
        simd::f32x4 v[4] = {va, vb, simd::add(va, vb), simd::mul(va, vb)};
        simd::f32x4s s[4] = {sa, sb, simd::add(sa, sb), simd::mul(sa, sb)};
        simd::transpose4(v[0], v[1], v[2], v[3]);
        simd::transpose4(s[0], s[1], s[2], s[3]);
        for (int i = 0; i < 4; ++i) {
            CHECK(same(v[i], s[i]));
        }
        float t[4];
        simd::store(t, s[1]);
        CHECK(std::memcmp(&t[0], &a[1], sizeof(float)) == 0);  // row 0 column 1 -> row 1 column 0
    }
}

namespace {

constexpr int kRw = 24;
constexpr int kRh = 12;

/// A root target whose transparent pixels keep non-zero colour bytes -- what a zero source-over
/// clears (un-premultiplying alpha 0 zeroes the colour) and a skipped pixel does not.
std::vector<uint8_t> patternRoot() {
    std::vector<uint8_t> px(static_cast<size_t>(kRw) * kRh * 4);
    for (size_t i = 0; i < px.size() / 4; ++i) {
        px[i * 4 + 0] = static_cast<uint8_t>(17 * i);
        px[i * 4 + 1] = static_cast<uint8_t>(29 * i + 3);
        px[i * 4 + 2] = static_cast<uint8_t>(41 * i + 7);
        px[i * 4 + 3] = static_cast<uint8_t>(i % 3 == 0 ? 0 : 60 * (i % 5));
    }
    return px;
}

/// Pushes a full-root layer at `opacity`, sets every pixel of rows `[y0, y1)` to `c` and pops it.
void drawRows(LayerStack& stack, const Target& root, float opacity, int y0, int y1, const Color& c) {
    stack.pushLayer(root, opacity, BlendMode::Normal, Rect{0, 0, kRw, kRh});
    Target layer = stack.currentTarget(root);
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < kRw; ++x) {
            layer.set(x, y, c);
        }
    }
    stack.popLayer(root);
}

}  // namespace

TEST_CASE("LayerStack: a reused pool surface is not cleared; undrawn rows composite as transparent (T-643)") {
    for (float opacity : {1.0f, 0.5f}) {
        CAPTURE(opacity);
        LayerStack stack;
        std::vector<uint8_t> warm = patternRoot();
        Target warmRoot = Target::fromSwTarget(SwTarget{warm.data(), kRw, kRh, kRw * 4});
        drawRows(stack, warmRoot, 1.0f, 0, kRh, Color{1, 0, 0, 1});  // The pool slot now holds red.
        REQUIRE(stack.poolSize() == 1);
        CHECK(stack.definedRows(0) == static_cast<size_t>(kRh));

        std::vector<uint8_t> got = patternRoot();
        Target root = Target::fromSwTarget(SwTarget{got.data(), kRw, kRh, kRw * 4});
        const Color blue{0, 0, 0.5f, 0.5f};
        drawRows(stack, root, opacity, 5, 7, blue);
        REQUIRE(stack.poolSize() == 1);
        CHECK(stack.definedRows(0) == 2u);  // Only the two drawn rows were ever cleared.

        // Expected: the same composite of an explicitly cleared layer, through the kernels popLayer
        // used before T-643 (source-over spans at opacity 1, the per-pixel loop otherwise).
        std::vector<uint8_t> want = patternRoot();
        std::vector<float> row(static_cast<size_t>(kRw) * 4, 0.0f);
        for (int y = 0; y < kRh; ++y) {
            for (int x = 0; x < kRw; ++x) {
                const bool drawn = y >= 5 && y < 7;
                row[static_cast<size_t>(x) * 4 + 0] = drawn ? blue.r : 0.0f;
                row[static_cast<size_t>(x) * 4 + 1] = drawn ? blue.g : 0.0f;
                row[static_cast<size_t>(x) * 4 + 2] = drawn ? blue.b : 0.0f;
                row[static_cast<size_t>(x) * 4 + 3] = drawn ? blue.a : 0.0f;
            }
            uint8_t* dst = want.data() + static_cast<size_t>(y) * kRw * 4;
            if (opacity == 1.0f) {
                swSimd().spanSrcOverU8(dst, row.data(), kRw, false);
            } else {
                referenceU8(dst, row.data(), kRw, opacity, BlendMode::Normal, false);
            }
        }
        CHECK(got == want);
    }
}

TEST_CASE(
    "LayerStack: a nested layer onto an undrawn row of its parent reads that row as transparent (T-643)") {
    LayerStack stack;
    std::vector<uint8_t> warm = patternRoot();
    Target warmRoot = Target::fromSwTarget(SwTarget{warm.data(), kRw, kRh, kRw * 4});
    // Two slots full of red, so neither the parent nor the child starts out zeroed.
    for (int depth = 0; depth < 2; ++depth) {
        stack.pushLayer(warmRoot, 1.0f, BlendMode::Normal, Rect{0, 0, kRw, kRh});
        Target layer = stack.currentTarget(warmRoot);
        for (int y = 0; y < kRh; ++y) {
            for (int x = 0; x < kRw; ++x) {
                layer.set(x, y, Color{1, 0, 0, 1});
            }
        }
    }
    stack.popLayer(warmRoot);
    stack.popLayer(warmRoot);
    REQUIRE(stack.poolSize() == 2);

    std::vector<uint8_t> got(static_cast<size_t>(kRw) * kRh * 4, 0);
    Target root = Target::fromSwTarget(SwTarget{got.data(), kRw, kRh, kRw * 4});
    stack.pushLayer(root, 1.0f, BlendMode::Normal, Rect{0, 0, kRw, kRh});    // parent: nothing drawn
    stack.pushLayer(root, 0.5f, BlendMode::Multiply, Rect{0, 0, kRw, kRh});  // child
    Target child = stack.currentTarget(root);
    for (int x = 0; x < kRw; ++x) {
        child.set(x, 3, Color{0, 0.6f, 0, 0.6f});
    }
    stack.popLayer(root);  // onto the parent's row 3, never drawn: transparent, not stale red
    CHECK(stack.currentTarget(root).get(4, 3).r == 0.0f);
    CHECK(stack.currentTarget(root).get(4, 2).a == 0.0f);
    stack.popLayer(root);
    for (int y = 0; y < kRh; ++y) {
        for (int x = 0; x < kRw; ++x) {
            const uint8_t* p = got.data() + (static_cast<size_t>(y) * kRw + static_cast<size_t>(x)) * 4;
            CAPTURE(y);
            CHECK(p[0] == 0);  // no red anywhere
            CHECK(p[3] == (y == 3 ? 77 : 0));
        }
    }
}

#include <cstdint>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "sw_simd_kernels.h"
#include "xt/simd.h"

/// docs/tasks/T-102.md "測試" 1 and 2: every `xt/simd.h` function against its scalar reference on
/// random input (floats within 2 ulp), and the three dispatched hot spots -- span blending, the
/// Gaussian blur box passes, float <-> RGBA8 conversion -- SIMD against scalar (bytes equal, or a
/// channel off by at most 1 for the blur).

using namespace xt;

namespace {

/// Deterministic xorshift, so a failure is reproducible without a seed to carry around.
struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    /// Uniform in [lo, hi].
    float unit(float lo = 0.0f, float hi = 1.0f) {
        return lo + (hi - lo) * (static_cast<float>(next() >> 8) / 16777216.0f);
    }
};

/// Distance in representable floats; `a == b` gives 0. Both arguments are finite and non-negative
/// in these tests, so the naive integer difference is the ulp distance.
int64_t ulpDiff(float a, float b) {
    int32_t ia = 0;
    int32_t ib = 0;
    std::memcpy(&ia, &a, sizeof(ia));
    std::memcpy(&ib, &b, sizeof(ib));
    if (ia < 0) {
        ia = static_cast<int32_t>(0x80000000u) - ia;
    }
    if (ib < 0) {
        ib = static_cast<int32_t>(0x80000000u) - ib;
    }
    int64_t d = static_cast<int64_t>(ia) - static_cast<int64_t>(ib);
    return d < 0 ? -d : d;
}

/// Bit pattern of a lane, for the mask-producing functions where NaN != NaN makes == useless.
uint32_t bits(float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

/// Stores a vector of either type into 4 floats.
template <class V>
void toArray(V v, float out[4]) {
    simd::store(out, v);
}

}  // namespace

TEST_CASE("simd: every f32x4 function matches its scalar reference within 2 ulp") {
    Rng rng;
    for (int iter = 0; iter < 512; ++iter) {
        float a[4];
        float b[4];
        float c[4];
        for (int i = 0; i < 4; ++i) {
            a[i] = rng.unit(-2.0f, 2.0f);
            b[i] = rng.unit(-2.0f, 2.0f);
            c[i] = rng.unit(-2.0f, 2.0f);
        }
        const float scalarS = rng.unit(-1.0f, 1.0f);
        uint8_t bytes[4];
        for (int i = 0; i < 4; ++i) {
            bytes[i] = static_cast<uint8_t>(rng.next() & 0xFFu);
        }

        const simd::f32x4 va = sw::simdk::vload<simd::f32x4>(a);
        const simd::f32x4 vb = sw::simdk::vload<simd::f32x4>(b);
        const simd::f32x4 vc = sw::simdk::vload<simd::f32x4>(c);
        const simd::f32x4s sa = sw::simdk::vload<simd::f32x4s>(a);
        const simd::f32x4s sb = sw::simdk::vload<simd::f32x4s>(b);
        const simd::f32x4s sc = sw::simdk::vload<simd::f32x4s>(c);

        float gotV[4];
        float gotS[4];
        auto sameWithin2Ulp = [&](const char* what) {
            for (int i = 0; i < 4; ++i) {
                INFO(what << " lane " << i << ": simd=" << gotV[i] << " scalar=" << gotS[i]);
                CHECK(ulpDiff(gotV[i], gotS[i]) <= 2);
            }
        };

        toArray(va, gotV);
        toArray(sa, gotS);
        sameWithin2Ulp("load/store");

        toArray(simd::splat(static_cast<const simd::f32x4*>(nullptr), scalarS), gotV);
        toArray(simd::splat(static_cast<const simd::f32x4s*>(nullptr), scalarS), gotS);
        sameWithin2Ulp("splat");

        toArray(simd::add(va, vb), gotV);
        toArray(simd::add(sa, sb), gotS);
        sameWithin2Ulp("add");

        toArray(simd::sub(va, vb), gotV);
        toArray(simd::sub(sa, sb), gotS);
        sameWithin2Ulp("sub");

        toArray(simd::mul(va, vb), gotV);
        toArray(simd::mul(sa, sb), gotS);
        sameWithin2Ulp("mul");

        toArray(simd::div(va, vb), gotV);
        toArray(simd::div(sa, sb), gotS);
        sameWithin2Ulp("div");

        toArray(simd::fma(va, vb, vc), gotV);
        toArray(simd::fma(sa, sb, sc), gotS);
        sameWithin2Ulp("fma");

        toArray(simd::min(va, vb), gotV);
        toArray(simd::min(sa, sb), gotS);
        sameWithin2Ulp("min");

        toArray(simd::max(va, vb), gotV);
        toArray(simd::max(sa, sb), gotS);
        sameWithin2Ulp("max");

        toArray(simd::splatW(va), gotV);
        toArray(simd::splatW(sa), gotS);
        sameWithin2Ulp("splatW");

        toArray(simd::loadU8(static_cast<const simd::f32x4*>(nullptr), bytes), gotV);
        toArray(simd::loadU8(static_cast<const simd::f32x4s*>(nullptr), bytes), gotS);
        sameWithin2Ulp("loadU8");

        // cmpGt produces a bit mask (NaN when set), so compare the patterns, not the values.
        toArray(simd::cmpGt(va, vb), gotV);
        toArray(simd::cmpGt(sa, sb), gotS);
        for (int i = 0; i < 4; ++i) {
            CHECK(bits(gotV[i]) == bits(gotS[i]));
        }

        toArray(simd::blend(va, vb, simd::cmpGt(va, vb)), gotV);
        toArray(simd::blend(sa, sb, simd::cmpGt(sa, sb)), gotS);
        sameWithin2Ulp("blend");

        uint8_t outV[4];
        uint8_t outS[4];
        simd::storeU8(outV, va);
        simd::storeU8(outS, sa);
        for (int i = 0; i < 4; ++i) {
            CHECK(static_cast<int>(outV[i]) == static_cast<int>(outS[i]));
        }
    }
}

TEST_CASE("simd: storeU8 clamps and rounds half up, like floatToByte") {
    const float in[4] = {-0.5f, 0.0f, 0.5f, 2.0f};
    uint8_t out[4];
    simd::storeU8(out, sw::simdk::vload<simd::f32x4>(in));
    CHECK(static_cast<int>(out[0]) == 0);
    CHECK(static_cast<int>(out[1]) == 0);
    CHECK(static_cast<int>(out[2]) == 128);
    CHECK(static_cast<int>(out[3]) == 255);
}

namespace {

/// The scalar table the dispatched one is checked against (what `XT_SW_FORCE_SCALAR=1` selects).
const xt::sw::SwSimdOps& scalarOps() {
    static const xt::sw::SwSimdOps ops = sw::simdk::makeOps<simd::f32x4s>("scalar");
    return ops;
}

std::vector<float> randomPremultipliedPixels(Rng& rng, int n) {
    std::vector<float> px(static_cast<size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        const float a = rng.unit();
        px[static_cast<size_t>(i) * 4 + 0] = rng.unit() * a;
        px[static_cast<size_t>(i) * 4 + 1] = rng.unit() * a;
        px[static_cast<size_t>(i) * 4 + 2] = rng.unit() * a;
        px[static_cast<size_t>(i) * 4 + 3] = a;
    }
    return px;
}

}  // namespace

TEST_CASE("simd hot spot (a): span blending matches the scalar kernels byte for byte") {
    Rng rng;
    const int n = 61;  // deliberately not a multiple of any vector width
    const std::vector<float> src = randomPremultipliedPixels(rng, n);

    for (int premulTarget = 0; premulTarget < 2; ++premulTarget) {
        const bool dstPremul = premulTarget != 0;

        std::vector<uint8_t> a(static_cast<size_t>(n) * 4);
        for (size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<uint8_t>(rng.next() & 0xFFu);
        }
        std::vector<uint8_t> b = a;
        xt::sw::swSimd().spanSrcOverU8(a.data(), src.data(), n, dstPremul);
        scalarOps().spanSrcOverU8(b.data(), src.data(), n, dstPremul);
        CHECK(a == b);

        const float solid[4] = {0.2f, 0.35f, 0.1f, 0.5f};
        for (size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<uint8_t>(rng.next() & 0xFFu);
        }
        b = a;
        xt::sw::swSimd().spanSolidOverU8(a.data(), n, solid, 0.5f, dstPremul);
        scalarOps().spanSolidOverU8(b.data(), n, solid, 0.5f, dstPremul);
        CHECK(a == b);
    }

    std::vector<float> fa = randomPremultipliedPixels(rng, n);
    std::vector<float> fb = fa;
    xt::sw::swSimd().spanSrcOverF(fa.data(), src.data(), n);
    scalarOps().spanSrcOverF(fb.data(), src.data(), n);
    for (size_t i = 0; i < fa.size(); ++i) {
        CHECK(ulpDiff(fa[i], fb[i]) <= 2);
    }

    const float solid[4] = {0.2f, 0.35f, 0.1f, 0.5f};
    fa = randomPremultipliedPixels(rng, n);
    fb = fa;
    xt::sw::swSimd().spanSolidOverF(fa.data(), n, solid, 0.5f);
    scalarOps().spanSolidOverF(fb.data(), n, solid, 0.5f);
    for (size_t i = 0; i < fa.size(); ++i) {
        CHECK(ulpDiff(fa[i], fb[i]) <= 2);
    }
}

TEST_CASE("simd hot spot (b): blur box passes match the scalar kernels within 1/255") {
    Rng rng;
    const int w = 37;
    const int h = 23;
    for (int channels : {1, 4}) {
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels);
        std::vector<float> src(n);
        for (size_t i = 0; i < n; ++i) {
            src[i] = rng.unit();
        }
        for (int radius : {1, 3, 8}) {
            std::vector<float> outSimd(n, 0.0f);
            std::vector<float> outScalar(n, 0.0f);
            xt::sw::swSimd().boxPassH(src.data(), outSimd.data(), w, h, channels, radius);
            scalarOps().boxPassH(src.data(), outScalar.data(), w, h, channels, radius);
            for (size_t i = 0; i < n; ++i) {
                // The blur's tolerance is one 8-bit channel step (docs/tasks/T-102.md test 2).
                CHECK(std::abs(outSimd[i] - outScalar[i]) <= 1.0f / 255.0f);
            }
            std::fill(outSimd.begin(), outSimd.end(), 0.0f);
            std::fill(outScalar.begin(), outScalar.end(), 0.0f);
            xt::sw::swSimd().boxPassV(src.data(), outSimd.data(), w, h, channels, radius);
            scalarOps().boxPassV(src.data(), outScalar.data(), w, h, channels, radius);
            for (size_t i = 0; i < n; ++i) {
                CHECK(std::abs(outSimd[i] - outScalar[i]) <= 1.0f / 255.0f);
            }
        }
    }
}

TEST_CASE("simd hot spot (c): float <-> RGBA8 conversion matches the scalar kernels exactly") {
    Rng rng;
    const int n = 53;
    const std::vector<float> src = randomPremultipliedPixels(rng, n);
    for (int premulTarget = 0; premulTarget < 2; ++premulTarget) {
        const bool premul = premulTarget != 0;

        std::vector<uint8_t> a(static_cast<size_t>(n) * 4, 0);
        std::vector<uint8_t> b(static_cast<size_t>(n) * 4, 0);
        xt::sw::swSimd().rowToU8(src.data(), a.data(), n, premul);
        scalarOps().rowToU8(src.data(), b.data(), n, premul);
        CHECK(a == b);

        std::vector<uint8_t> bytes(static_cast<size_t>(n) * 4);
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(rng.next() & 0xFFu);
        }
        std::vector<float> fa(static_cast<size_t>(n) * 4, 0.0f);
        std::vector<float> fb(static_cast<size_t>(n) * 4, 0.0f);
        xt::sw::swSimd().rowFromU8(bytes.data(), fa.data(), n, premul);
        scalarOps().rowFromU8(bytes.data(), fb.data(), n, premul);
        for (size_t i = 0; i < fa.size(); ++i) {
            CHECK(ulpDiff(fa[i], fb[i]) <= 2);
        }
    }
}

TEST_CASE("simd: a table is selected and names itself") {
    const char* name = xt::sw::swSimd().name;
    REQUIRE(name != nullptr);
    const bool known =
        std::strcmp(name, "scalar") == 0 || std::strcmp(name, "sse2") == 0 || std::strcmp(name, "avx2") == 0;
    INFO("selected SIMD table: " << name);
    CHECK(known);
}

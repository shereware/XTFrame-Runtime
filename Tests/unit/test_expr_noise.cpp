#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/expr_noise.h"

/// T-766 (Docs/tasks/T-766.md; T-776 item 5a): xt/expr_noise.h -- the noise, wiggle and random
/// functions the compiler and the runtime share -- pinned by a table of outputs for fixed inputs. The
/// runtime task checks the same table, so both sides stay identical bit for bit. Floats are compared
/// by their bit patterns.

namespace {

struct Row {
    const char* what;
    uint32_t bits;
};

uint32_t bits(float f) { return std::bit_cast<uint32_t>(f); }

/// The outputs, in table order.
std::vector<Row> compute() {
    std::vector<Row> r;
    r.push_back({"exprHash(0)", xt::exprHash(0)});
    r.push_back({"exprHash(1)", xt::exprHash(1)});
    r.push_back({"exprHash(12345)", xt::exprHash(12345)});
    r.push_back({"exprHash(0xFFFFFFFF)", xt::exprHash(0xFFFFFFFFu)});
    r.push_back({"exprMixSeed(1234, 0)", xt::exprMixSeed(1234, 0)});
    r.push_back({"exprMixSeed(1234, 1)", xt::exprMixSeed(1234, 1)});
    r.push_back({"exprMixSeed(0, 7)", xt::exprMixSeed(0, 7)});
    r.push_back({"exprLattice(-3, 0)", bits(xt::exprLattice(-3, 0))});
    r.push_back({"exprLattice(0, 99)", bits(xt::exprLattice(0, 99))});
    r.push_back({"exprLattice(7, 99)", bits(xt::exprLattice(7, 99))});
    r.push_back({"exprFade(0.3)", bits(xt::exprFade(0.3f))});
    r.push_back({"exprNoise(-2.25, 7)", bits(xt::exprNoise(-2.25f, 7))});
    r.push_back({"exprNoise(0, 7)", bits(xt::exprNoise(0.0f, 7))});
    r.push_back({"exprNoise(0.5, 7)", bits(xt::exprNoise(0.5f, 7))});
    r.push_back({"exprNoise(1.75, 7)", bits(xt::exprNoise(1.75f, 7))});
    r.push_back({"exprNoise(1000.125, 7)", bits(xt::exprNoise(1000.125f, 7))});
    r.push_back({"exprPeriodicNoise(0, 5, 7)", bits(xt::exprPeriodicNoise(0.0f, 5, 7))});
    r.push_back({"exprPeriodicNoise(2.5, 5, 7)", bits(xt::exprPeriodicNoise(2.5f, 5, 7))});
    r.push_back({"exprPeriodicNoise(5, 5, 7)", bits(xt::exprPeriodicNoise(5.0f, 5, 7))});
    r.push_back({"exprPeriodicNoise(-1.5, 5, 7)", bits(xt::exprPeriodicNoise(-1.5f, 5, 7))});
    r.push_back({"exprWigglePeriod(2, 15)", static_cast<uint32_t>(xt::exprWigglePeriod(2.0f, 15.0f))});
    r.push_back({"exprWigglePeriod(0.01, 15)", static_cast<uint32_t>(xt::exprWigglePeriod(0.01f, 15.0f))});
    r.push_back({"exprWigglePeriod(2, 0)", static_cast<uint32_t>(xt::exprWigglePeriod(2.0f, 0.0f))});
    r.push_back({"exprWigglePeriod(-2.6, 1)", static_cast<uint32_t>(xt::exprWigglePeriod(-2.6f, 1.0f))});
    r.push_back({"exprWiggleNoise(0, 2, 15, 99)", bits(xt::exprWiggleNoise(0.0f, 2.0f, 15.0f, 99))});
    r.push_back({"exprWiggleNoise(1.234, 2, 15, 99)", bits(xt::exprWiggleNoise(1.234f, 2.0f, 15.0f, 99))});
    r.push_back({"exprWiggleNoise(7.5, 2, 15, 99)", bits(xt::exprWiggleNoise(7.5f, 2.0f, 15.0f, 99))});
    r.push_back({"exprWiggleNoise(15, 2, 15, 99)", bits(xt::exprWiggleNoise(15.0f, 2.0f, 15.0f, 99))});
    r.push_back({"exprWiggleNoise(1.234, 2, 15, 99, 3, 0.5)",
                 bits(xt::exprWiggleNoise(1.234f, 2.0f, 15.0f, 99, 3, 0.5f))});
    r.push_back({"exprWiggleNoise(0.3, 2, 0, 99)", bits(xt::exprWiggleNoise(0.3f, 2.0f, 0.0f, 99))});
    r.push_back({"exprWiggleNoise(2.2, -2, 0, 99)", bits(xt::exprWiggleNoise(2.2f, -2.0f, 0.0f, 99))});
    r.push_back({"exprRandomFrame(0.5)", xt::exprRandomFrame(0.5f)});
    r.push_back({"exprRandomFrame(1/60)", xt::exprRandomFrame(1.0f / 60.0f)});
    r.push_back({"exprRandomSeed(5, 30, 2, false)", xt::exprRandomSeed(5, 30, 2, false)});
    r.push_back({"exprRandomSeed(5, 30, 2, true)", xt::exprRandomSeed(5, 30, 2, true)});
    r.push_back({"exprRandom01(0)", bits(xt::exprRandom01(0))});
    r.push_back({"exprRandom01(42)", bits(xt::exprRandom01(42))});
    r.push_back({"exprSeedFromNumber(-2.5)", xt::exprSeedFromNumber(-2.5f)});
    return r;
}

// The expected outputs (T-776's runtime test copies this table).
constexpr uint32_t kExpected[] = {
    0x00000000u,  // exprHash(0)
    0x688990C0u,  // exprHash(1)
    0x912EFCF7u,  // exprHash(12345)
    0x6768824Au,  // exprHash(0xFFFFFFFF)
    0x16B272E8u,  // exprMixSeed(1234, 0)
    0x943B4FB6u,  // exprMixSeed(1234, 1)
    0x5E00D4B8u,  // exprMixSeed(0, 7)
    0x3F7A91AEu,  // exprLattice(-3, 0)
    0xBDAED958u,  // exprLattice(0, 99)
    0x3E903CE8u,  // exprLattice(7, 99)
    0x3E26FE73u,  // exprFade(0.3)
    0x3E1D3D3Cu,  // exprNoise(-2.25, 7)
    0x3F50E8BCu,  // exprNoise(0, 7)
    0x3E062828u,  // exprNoise(0.5, 7)
    0x3F2E2D74u,  // exprNoise(1.75, 7)
    0xBF2D1665u,  // exprNoise(1000.125, 7)
    0x3F50E8BCu,  // exprPeriodicNoise(0, 5, 7)
    0x3F0B556Au,  // exprPeriodicNoise(2.5, 5, 7)
    0x3F50E8BCu,  // exprPeriodicNoise(5, 5, 7)
    0xBDB5921Cu,  // exprPeriodicNoise(-1.5, 5, 7)
    0x0000001Eu,  // exprWigglePeriod(2, 15)
    0x00000001u,  // exprWigglePeriod(0.01, 15)
    0x00000000u,  // exprWigglePeriod(2, 0)
    0x00000003u,  // exprWigglePeriod(-2.6, 1)
    0x3DB57740u,  // exprWiggleNoise(0, 2, 15, 99)
    0x3F3EB0C8u,  // exprWiggleNoise(1.234, 2, 15, 99)
    0x3F353948u,  // exprWiggleNoise(7.5, 2, 15, 99)
    0x3DB57740u,  // exprWiggleNoise(15, 2, 15, 99)
    0x3F3981C9u,  // exprWiggleNoise(1.234, 2, 15, 99, 3, 0.5)
    0x3F07D358u,  // exprWiggleNoise(0.3, 2, 0, 99)
    0x3EC59630u,  // exprWiggleNoise(2.2, -2, 0, 99)
    0x0000001Eu,  // exprRandomFrame(0.5)
    0x00000001u,  // exprRandomFrame(1/60)
    0xC9A49FDFu,  // exprRandomSeed(5, 30, 2, false)
    0xE7AD70B6u,  // exprRandomSeed(5, 30, 2, true)
    0x00000000u,  // exprRandom01(0)
    0x3DB93998u,  // exprRandom01(42)
    0xFFFFFFFDu,  // exprSeedFromNumber(-2.5)
};

}  // namespace

TEST_CASE("expr noise: fixed outputs for fixed inputs") {
    const std::vector<Row> rows = compute();
    if (std::size(kExpected) != rows.size()) {
        std::string table;
        for (const Row& row : rows) {
            char line[96];
            std::snprintf(line, sizeof line, "    0x%08Xu,  // %s\n", row.bits, row.what);
            table += line;
        }
        MESSAGE("expected table:\n" << table);
        FAIL("the table does not match the inputs");
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        INFO(rows[i].what);
        CHECK(rows[i].bits == kExpected[i]);
    }
}

TEST_CASE("expr noise: properties") {
    // Periodic wiggle: time 0 and the duration agree bit for bit, for every octave count.
    for (int oct = 1; oct <= 4; ++oct) {
        CHECK(bits(xt::exprWiggleNoise(0.0f, 2.0f, 15.0f, 5, oct)) ==
              bits(xt::exprWiggleNoise(15.0f, 2.0f, 15.0f, 5, oct)));
        CHECK(bits(xt::exprWiggleNoise(0.0f, 0.37f, 4.0f, 5, oct)) ==
              bits(xt::exprWiggleNoise(4.0f, 0.37f, 4.0f, 5, oct)));
    }
    // Ranges.
    for (int i = 0; i < 1000; ++i) {
        const float x = static_cast<float>(i) * 0.0137f - 5.0f;
        const float n = xt::exprNoise(x, 3);
        CHECK(n >= -1.0f);
        CHECK(n <= 1.0f);
        const float r = xt::exprRandom01(static_cast<uint32_t>(i));
        CHECK(r >= 0.0f);
        CHECK(r < 1.0f);
    }
    // Non-finite inputs never misbehave.
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(xt::exprWigglePeriod(inf, 1.0f) == 0);
    CHECK(xt::exprFloorToInt(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(xt::exprFloorToInt(1e30f) == INT32_MAX - 1);
    CHECK(xt::exprFloorToInt(-1e30f) == INT32_MIN);
}

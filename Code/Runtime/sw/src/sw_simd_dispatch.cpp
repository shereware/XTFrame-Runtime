#include <cstdlib>

#include "sw_simd_kernels.h"

#if defined(_MSC_VER)
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>  // T-240: x86 only.
#endif

/// Run-time selection between the scalar, SSE2 and AVX2 kernel tables (Docs/tasks/T-102.md
/// step 3). See sw_simd.h.

namespace xt::sw {

namespace {

/// True when the CPU reports AVX2 (CPUID leaf 7, EBX bit 5) *and* the OS saves YMM state
/// (CPUID leaf 1: OSXSAVE = ECX bit 27, AVX = ECX bit 28, then XCR0 bits 1 and 2).
[[maybe_unused]] bool cpuHasAvx2() {
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    int regs[4] = {0, 0, 0, 0};
#if defined(_MSC_VER)
    __cpuid(regs, 0);
#else
    __cpuid(0, regs[0], regs[1], regs[2], regs[3]);
#endif
    if (regs[0] < 7) {
        return false;
    }
#if defined(_MSC_VER)
    __cpuid(regs, 1);
#else
    __cpuid(1, regs[0], regs[1], regs[2], regs[3]);
#endif
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) {
        return false;
    }
#if defined(_MSC_VER)
    const unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned int lo = 0;
    unsigned int hi = 0;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
    const unsigned long long xcr0 = (static_cast<unsigned long long>(hi) << 32) | lo;
#endif
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return false;  // XMM/YMM state not enabled by the OS.
    }
#if defined(_MSC_VER)
    __cpuidex(regs, 7, 0);
#else
    __cpuid_count(7, 0, regs[0], regs[1], regs[2], regs[3]);
#endif
    return (regs[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

/// True when environment variable `name` is set to "1". Two are read, both test/measurement
/// switches: `XT_SW_FORCE_SCALAR` forces the scalar table (Docs/tasks/T-102.md step 3) and
/// `XT_SW_NO_AVX2` holds the selection at the SSE2 baseline, which is how T-102 measured AVX2
/// against SSE2 on one build.
bool envFlag(const char* name) {
#if defined(_MSC_VER)
    size_t len = 0;
    char buf[8] = {0};
    if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) {
        return false;
    }
    return buf[0] == '1';
#else
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1';
#endif
}

SwSimdOps selectOps() {
    if (envFlag("XT_SW_FORCE_SCALAR")) {
        return simdk::makeOps<simd::f32x4s>("scalar");
    }
#if XT_SIMD_SSE2
    if (cpuHasAvx2() && !envFlag("XT_SW_NO_AVX2")) {
        return swSimdAvx2Ops();
    }
    return simdk::makeOps<simd::f32x4>("sse2");
#else
    return simdk::makeOps<simd::f32x4>("scalar");
#endif
}

}  // namespace

const SwSimdOps& swSimd() {
    static const SwSimdOps ops = selectOps();
    return ops;
}

}  // namespace xt::sw

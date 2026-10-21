#pragma once

/// double nowSeconds(): a monotonic wall clock for tests/perf's threshold measurements
/// (test_perf_thresholds.cpp). <chrono> is banned by docs/design/14-coding-standards.md §14.1
/// (libstdc++ 14's <chrono> pulls in <format>, which clang 15 cannot compile), so this uses
/// QueryPerformanceCounter on Windows and clock_gettime elsewhere, exactly like the rest of the
/// codebase avoids <chrono> (see e.g. compile/src/compile_lottie.cpp's <ctime> use).

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace xt::perf {

/// Seconds, monotonic, arbitrary epoch -- only differences between two calls are meaningful.
inline double nowSeconds() {
#if defined(_WIN32)
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

}  // namespace xt::perf

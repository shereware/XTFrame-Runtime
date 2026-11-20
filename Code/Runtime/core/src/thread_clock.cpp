#include "xt/thread_clock.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

/// T-750: see thread_clock.h (moved from Code/Tools/gpu-probe/probe_timing.h, T-734).

namespace xt {

namespace {
#if defined(_WIN32)
uint64_t threadCycles() {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    return static_cast<uint64_t>(c);
}
#endif
}  // namespace

void ThreadClock::calibrate() {
#if defined(_WIN32)
    using Clock = std::chrono::steady_clock;
    volatile uint64_t sink = 0;
    double best = 0, total = 0;
    for (int w = 0; w < 8; ++w) {
        const uint64_t c0 = threadCycles();
        const auto t0 = Clock::now();
        double ms = 0;
        while (ms < 25.0) {
            for (int i = 0; i < 10000; ++i) {
                sink = sink + static_cast<uint64_t>(i);
            }
            ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }
        best = std::max(best, static_cast<double>(threadCycles() - c0) / ms);
        total += ms;
    }
    m_cyclesPerMs = best;
    m_calibrationMs = total;
#else
    m_cyclesPerMs = 1;  // not used: marks the clock calibrated
#endif
}

bool ThreadClock::calibrated() const {
#if defined(_WIN32)
    return m_cyclesPerMs > 0;
#else
    return true;
#endif
}

double ThreadClock::nowMs() const {
#if defined(_WIN32)
    return m_cyclesPerMs > 0 ? static_cast<double>(threadCycles()) / m_cyclesPerMs : 0.0;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
#endif
}

size_t ThreadClock::describe(char* buffer, size_t size) const {
    if (buffer == nullptr || size == 0) {
        return 0;
    }
#if defined(_WIN32)
    const int n = std::snprintf(
        buffer, size,
        "thread CPU = QueryThreadCycleTime / %.1f cycles per us (best of 8 windows, %.0f ms busy loop)",
        m_cyclesPerMs / 1000.0, m_calibrationMs);
#else
    const int n = std::snprintf(buffer, size, "thread CPU = CLOCK_THREAD_CPUTIME_ID");
#endif
    if (n < 0) {
        buffer[0] = '\0';
        return 0;
    }
    return std::min(static_cast<size_t>(n), size - 1);
}

}  // namespace xt

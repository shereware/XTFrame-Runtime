#pragma once

#include <cstddef>

/// T-750 (Docs/design/57-measurement.md §57.2 item 4, 10 §10.19): the calling thread's CPU time, moved
/// here from xtframe-gpu-probe (probe_timing.h) so the core's PerfTracker and every tool share it.

namespace xt {

/// The calling thread's CPU time in milliseconds. Windows: QueryThreadCycleTime, with cycles per
/// millisecond calibrated against steady_clock by a busy loop of about 200 ms (calibrate()); elsewhere
/// CLOCK_THREAD_CPUTIME_ID (calibrate() does nothing). Before calibrate() on Windows nowMs() is 0.
class ThreadClock {
public:
    /// Measures the cycle rate (Windows: eight 25 ms windows, the highest rate wins -- a window in
    /// which the thread was preempted counts fewer cycles per wall millisecond). Costs ~200 ms.
    void calibrate();
    /// True once calibrate() ran (always true where no calibration is needed).
    bool calibrated() const;
    /// The calling thread's CPU time so far, in milliseconds.
    double nowMs() const;
    /// One line naming the clock and its calibration, NUL-terminated into `buffer`; returns the
    /// length written (truncated to `size - 1`).
    size_t describe(char* buffer, size_t size) const;

private:
    double m_cyclesPerMs = 0;
    double m_calibrationMs = 0;
};

}  // namespace xt

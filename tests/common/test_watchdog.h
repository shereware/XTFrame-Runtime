// Per-test-case watchdog shared by every doctest `main` (T-267e, moved here from
// tests/unit/test_main.cpp by T-267d so the suites with their own `main` -- tests/editor_qt,
// tests/gl -- get it too).
//
// A test case that hangs (a deadlock, a lost wake-up) used to stall the whole run until the gate's
// own timeout killed the process, with nothing saying which case it was. A watchdog thread now names
// the case on stderr ("TEST TIMEOUT: ...") and ends the process (exit code 124) once a single case
// has run longer than XT_TEST_CASE_TIMEOUT seconds (default 600; 0 turns the watchdog off).
//
// Include this header in exactly one .cpp of a test executable -- the one holding `main`, after
// "doctest.h" with DOCTEST_CONFIG_IMPLEMENT -- because it registers a doctest listener; then call
// xt::test::startWatchdog() at the start of `main`.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "doctest.h"

namespace xt::test::watchdog_detail {

struct RunningCase {
    std::mutex mutex;
    std::string name;  // "file(line): name" of the case being run; empty between cases.
    std::chrono::steady_clock::time_point started;
};

// Never destroyed: the detached watchdog thread may still look at it while the process exits.
inline RunningCase& runningCase() {
    static RunningCase* const state = new RunningCase;
    return *state;
}

struct WatchdogListener : doctest::IReporter {
    explicit WatchdogListener(const doctest::ContextOptions&) {}

    void test_case_start(const doctest::TestCaseData& in) override {
        RunningCase& state = runningCase();
        const std::lock_guard<std::mutex> lock(state.mutex);
        state.name = std::string(in.m_file.c_str()) + "(" + std::to_string(in.m_line) + "): " + in.m_name;
        state.started = std::chrono::steady_clock::now();
    }
    void test_case_end(const doctest::CurrentTestCaseStats&) override {
        RunningCase& state = runningCase();
        const std::lock_guard<std::mutex> lock(state.mutex);
        state.name.clear();
    }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("xt_watchdog", 1, WatchdogListener);

/// XT_TEST_CASE_TIMEOUT in seconds; 600 when unset or not a number.
inline long caseTimeoutSeconds() {
    std::string text;
#ifdef _MSC_VER
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "XT_TEST_CASE_TIMEOUT") == 0 && value != nullptr) {
        text = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv("XT_TEST_CASE_TIMEOUT")) {
        text = value;
    }
#endif
    if (text.empty()) {
        return 600;
    }
    char* end = nullptr;
    const long seconds = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && seconds >= 0 ? seconds : 600;
}

}  // namespace xt::test::watchdog_detail

namespace xt::test {

/// Starts the watchdog thread (nothing when XT_TEST_CASE_TIMEOUT is 0).
inline void startWatchdog() {
    using namespace watchdog_detail;
    const long limit = caseTimeoutSeconds();
    if (limit <= 0) {
        return;
    }
    std::thread([limit] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            RunningCase& state = runningCase();
            const std::lock_guard<std::mutex> lock(state.mutex);
            if (state.name.empty()) {
                continue;
            }
            const auto elapsed = std::chrono::steady_clock::now() - state.started;
            if (elapsed > std::chrono::seconds(limit)) {
                std::fprintf(stderr, "\nTEST TIMEOUT: still running after %ld s: %s\n", limit,
                             state.name.c_str());
                std::fflush(stderr);
                std::fflush(stdout);
                std::_Exit(124);
            }
        }
    }).detach();
}

}  // namespace xt::test

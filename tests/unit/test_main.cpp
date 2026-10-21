#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

// Relative, not <test_temp.h>: devtools/agent's xt_agent_tests compiles this file too and does not
// have tests/common on its include path.
#include "../common/test_temp.h"
#include "../common/test_watchdog.h"

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

// Shared doctest entry point for every XTFrame test executable. On MSVC the Debug CRT reports
// assertion failures and abort() through a modal dialog, which silently hangs an unattended run
// (CI, agents); route them to stderr instead so a failing test terminates the process.
// T-267e: the per-case watchdog (tests/common/test_watchdog.h). T-267d: a passing run removes the
// process's temp root (tests/common/test_temp.h).
int main(int argc, char** argv) {
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    xt::test::startWatchdog();
    return xt::test::finishTempDir(doctest::Context(argc, argv).run());
}

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "test_temp.h"
#include "test_watchdog.h"

#ifdef _MSC_VER
#include <crtdbg.h>

#include <cstdlib>
#endif

// Same entry point as tests/unit/test_main.cpp: on MSVC route Debug-CRT assertion dialogs to
// stderr so an unattended run terminates instead of hanging; the per-case watchdog and the temp
// root clean-up (T-267d).
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

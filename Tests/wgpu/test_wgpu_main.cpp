#define DOCTEST_CONFIG_IMPLEMENT
#include <cstdio>

#include "doctest.h"
#include "test_temp.h"
#include "test_watchdog.h"
#include "test_wgpu_common.h"

#ifdef _MSC_VER
#include <crtdbg.h>

#include <cstdlib>
#endif

namespace xt::wgpu::test {

namespace {
WgpuInit g_init;
}

const WgpuInit& sharedInit() { return g_init; }

}  // namespace xt::wgpu::test

// Docs/design/10-backends.md §10.10.3: one headless device for the whole executable; without an
// adapter (no GPU, no driver, a sandbox) the executable prints `SKIP: no WebGPU adapter` and exits 0.
// XT_WGPU_FALLBACK=1 lets it use a software adapter (WARP, lavapipe) instead.
int main(int argc, char** argv) {
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    xt::wgpu::test::g_init = xt::wgpu::WgpuInit::headless();
    if (xt::wgpu::test::g_init.device == nullptr) {
        std::printf("SKIP: no WebGPU adapter (%s)\n", xt::wgpu::test::g_init.error.c_str());
        return 0;
    }
    xt::test::startWatchdog();
    const int rc = xt::test::finishTempDir(doctest::Context(argc, argv).run());
    xt::wgpu::test::g_init.release();
    return rc;
}

// T-747 (Docs/design/57-measurement.md §57.3, Docs/design/23-host-integration.md §23.ag item 4):
// the test suites' switch for the visual-generation check. Every xt::Context constructed while the
// environment variable XT_VERIFY_VISUAL_GENERATION is `1` compares, after each render(), "did the
// command list change?" with "did a painted Document's visual generation move, or was the previous
// list clock-dependent?", and aborts the process with the first differing command and its node when
// the list changed with no generation moving (a write site that missed its cause).
//
// T-748 (23 §23.ag item 4): the check is the static skip's verify mode -- a Document that would
// skip its paint is painted anyway and compared with its previous commands.
//
// Call xt::test::enableVisualCheck() at the start of `main` of a suite that should run with the
// check (every executable on Tests/unit/test_main.cpp, xt_golden, xt_gpu_tests, xt_gl_tests,
// xt_wgpu_tests, xt_backends_tests, xt_editor_qt_tests; Tools/Scripts/corpus_check.py sets the
// variable for xtframe-view, xtframe-bench sets it itself). A value already in the environment
// wins, so XT_VERIFY_VISUAL_GENERATION=0 turns it off for a run.
#pragma once

#include <cstdlib>

namespace xt::test {

inline void enableVisualCheck() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t size = 0;
    const bool set = _dupenv_s(&value, &size, "XT_VERIFY_VISUAL_GENERATION") == 0 && value != nullptr;
    std::free(value);
    if (!set) {
        _putenv_s("XT_VERIFY_VISUAL_GENERATION", "1");
    }
#else
    setenv("XT_VERIFY_VISUAL_GENERATION", "1", /*overwrite=*/0);
#endif
}

}  // namespace xt::test

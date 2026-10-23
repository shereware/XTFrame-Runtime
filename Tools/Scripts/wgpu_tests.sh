#!/usr/bin/env bash
# T-441 (Docs/design/10-backends.md §10.10.3, Docs/design/15-testing.md §15.8): runs the WebGPU tests
# of a configured and built tree through ctest -- xt_wgpu_tests, xt_golden_wgpu (xt_golden --backend
# wgpu) and xt_backends_tests -- and fails when a WebGPU test found no adapter. Those executables
# print `SKIP: ...` and exit 0 without one (CI runners without a GPU), so a plain ctest pass does not
# prove that they ran; the CI legs and Tools/Scripts/local_ci.sh call this script instead.
#
#   Tools/Scripts/wgpu_tests.sh [--allow-skip] <ctest arguments...>
#     Tools/Scripts/wgpu_tests.sh --preset vs2022-release
#     XT_WGPU_BACKEND=vulkan Tools/Scripts/wgpu_tests.sh --preset vs2022-release
#     XT_WGPU_FALLBACK=1 Tools/Scripts/wgpu_tests.sh --test-dir build/wsl-gcc
#
# --allow-skip: a missing adapter prints the SKIP line and exits 0 (a machine that may have none).
# XT_WGPU_BACKEND (vulkan | d3d12 | metal | gl) and XT_WGPU_FALLBACK=1 (allow a software adapter:
# WARP, lavapipe) pass through to the tests (Code/Runtime/wgpu/src/wgpu_init.cpp).
# No test selected (a tree configured without xt_wgpu) is a failure. Exit status: ctest's, or 1.
set -u
ALLOW_SKIP=0
if [ "${1:-}" = "--allow-skip" ]; then
    ALLOW_SKIP=1
    shift
fi
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
echo "wgpu tests: XT_WGPU_BACKEND=${XT_WGPU_BACKEND:-<default>} XT_WGPU_FALLBACK=${XT_WGPU_FALLBACK:-0}"
ctest "$@" -R '^(xt_wgpu_tests|xt_golden_wgpu|xt_backends_tests)$' --no-tests=error -V > "$LOG" 2>&1
rc=$?
# ctest -V prefixes each line of a test's output with "<test number>: ".
grep -E '^[0-9]+: (xt_golden: wgpu adapter:|SKIP: )|tests passed|tests failed|No tests were found' "$LOG" | sed 's/^/  /'
if [ "$rc" -ne 0 ]; then
    echo "wgpu tests: FAILED (ctest exit $rc); full output:"
    cat "$LOG"
    exit "$rc"
fi
if grep -qE '^[0-9]+: SKIP: ' "$LOG"; then
    if [ "$ALLOW_SKIP" = 1 ]; then
        echo "wgpu tests: SKIP (no WebGPU adapter; --allow-skip)"
        exit 0
    fi
    echo "wgpu tests: FAILED (no WebGPU adapter; set XT_WGPU_FALLBACK=1 for a software adapter)"
    exit 1
fi
echo "wgpu tests: PASS"

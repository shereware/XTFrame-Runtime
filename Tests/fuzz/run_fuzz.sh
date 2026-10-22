#!/usr/bin/env bash
# Runs libFuzzer targets for a fixed time budget each against their seed corpus, then preserves any
# crashing/timeout/OOM/leak inputs they found. Before fuzzing, every input under
# Tests/fuzz/regressions/<target>/ (fixed findings) is replayed once; a failing replay fails the
# run. See Docs/design/15-testing.md §15.5, Docs/tasks/T-39.md, Docs/tasks/T-267b.md.
#
# Usage: Tests/fuzz/run_fuzz.sh <target|all> <seconds> [build_dir]
#   target: ir xt lottie xtcl xts svg xtfx yaml image bundle_json import; `all` runs every one.
#   build_dir defaults to build/wsl-fuzz (relative to the repo root), matching
#   Tools/Scripts/local_ci.sh --fuzz and the sheet's WSL configure command.
#   XT_FUZZ_TIMEOUT (default 10): libFuzzer's per-input -timeout in seconds; a slower input is a
#   `timeout-*` finding.
# Exit status: 0 when every replay passed and no target found anything, else non-zero.
# Not a C++ file: never run clang-format on it.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ALL_TARGETS="ir xt lottie xtcl xts svg xtfx yaml image bundle_json import"

TARGET="${1:-}"
SECONDS_BUDGET="${2:-60}"
BUILD_DIR="${3:-$ROOT/build/wsl-fuzz}"
# Made absolute up front: the run below cd's into a scratch directory, and a relative BUILD_DIR
# (the common case -- see usage above) would otherwise stop resolving after that.
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$(pwd)/$BUILD_DIR" ;;
esac

usage() {
    echo "usage: $0 <$(echo "$ALL_TARGETS" | tr ' ' '|')|all> <seconds> [build_dir]" >&2
    exit 2
}

if [ "$TARGET" = "all" ]; then
    TARGETS="$ALL_TARGETS"
else
    case " $ALL_TARGETS " in
        *" $TARGET "*) TARGETS="$TARGET" ;;
        *) usage ;;
    esac
fi

OUT_DIR="$BUILD_DIR/fuzz-out"
mkdir -p "$OUT_DIR"

# run_one <target>: replay, then fuzz; returns non-zero on any failure.
run_one() {
    local target="$1"
    # T-430: the executables are in bin/<platform>_<arch>/<Config>/tests/, named by the build tree's
    # Tests/fuzz/fuzz-bin-dir.txt; a build tree without it keeps them in Tests/fuzz/.
    local bin_dir="$BUILD_DIR/tests/fuzz"
    [ -f "$BUILD_DIR/tests/fuzz/fuzz-bin-dir.txt" ] && bin_dir="$(head -1 "$BUILD_DIR/tests/fuzz/fuzz-bin-dir.txt" | tr -d '\r')"
    local bin="$bin_dir/fuzz_$target"
    local corpus="$BUILD_DIR/tests/fuzz/corpus/$target"
    local regressions="$ROOT/tests/fuzz/regressions/$target"
    local status=0

    if [ ! -x "$bin" ]; then
        echo "$0: fuzz binary not found: $bin (configure/build with -DXT_BUILD_FUZZ=ON first)" >&2
        return 1
    fi
    mkdir -p "$corpus"

    # libFuzzer writes crash-*/oom-*/timeout-*/leak-* artifacts into its current directory; run it
    # from a scratch directory so a crash's minimized reproducer can be told apart from anything the
    # corpus directory already holds, then copy whatever it produced into build/fuzz-out/. The
    # targets that write their input to disk use TMPDIR, which points into it as well.
    local run_dir
    run_dir="$(mktemp -d)"

    local replay=()
    if [ -d "$regressions" ]; then
        for f in "$regressions"/*; do
            [ -f "$f" ] && replay+=("$f")
        done
    fi
    if [ "${#replay[@]}" -gt 0 ]; then
        echo "--- $target: replaying ${#replay[@]} regression input(s)"
        if ! (cd "$run_dir" && TMPDIR="$run_dir" "$bin" -timeout="${XT_FUZZ_TIMEOUT:-10}" "${replay[@]}"); then
            echo "$0: $target: a regression input failed" >&2
            status=1
        fi
    fi

    if [ "$status" -eq 0 ]; then
        echo "--- $target: fuzzing for ${SECONDS_BUDGET}s"
        (cd "$run_dir" && TMPDIR="$run_dir" "$bin" "$corpus" -max_total_time="$SECONDS_BUDGET" -max_len=1048576 \
            -timeout="${XT_FUZZ_TIMEOUT:-10}")
        status=$?
    fi

    local found=0
    for f in "$run_dir"/crash-* "$run_dir"/oom-* "$run_dir"/timeout-* "$run_dir"/leak-*; do
        [ -e "$f" ] || continue
        cp "$f" "$OUT_DIR/${target}-$(basename "$f")"
        found=$((found + 1))
    done
    rm -rf "$run_dir"

    if [ "$found" -gt 0 ]; then
        echo "$0: $target: $found crashing input(s) copied to $OUT_DIR"
        status=1
    fi
    return "$status"
}

fail=0
for target in $TARGETS; do
    run_one "$target" || fail=1
done
exit "$fail"

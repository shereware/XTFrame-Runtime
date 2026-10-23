#!/usr/bin/env bash
# Developer/agent check for one worktree: build the given targets with the vs2022-editor preset
# (configuring first when needed), delete the test executables so MSBuild relinks them, then run
# each test suite once with a timeout so a hung process never blocks the caller.
#   Tools/Scripts/dev_check.sh [--release] [--no-build] [--agent] <target>... -- <suite>...
#   suites: editor | editor_service | editor_qt | editor_qt_slow | editor_qt_gl | flow | bundle | history | core | golden |
#           gpu | gl | wgpu | backends | text | agent | import | lottie | symbols | templates | headers | layering | testtemp | import-py | ctest | corpus | ir |
#           roundtrip | shape
#          (ctest = ctest -E '^xt_.*_tests$', the command-line tests (T-267d); headers = Tools/Scripts/check_public_headers.sh, no build;
#           layering = Tools/Scripts/check_layering.sh (T-274, R never includes / links E) and, since T-267d,
#           Tools/Scripts/check_test_temp.sh too (both must pass), no build;
#           testtemp = Tools/Scripts/check_test_temp.sh alone (T-267d, no test uses the system temp directory
#           directly), no build;
#           editor_qt runs xt_editor_qt_tests without the doctest suites `gl` and `slow` (T-361: editor_qt_slow
#           runs the separate xt_editor_qt_tests_slow, built from the test files holding `slow` cases), and also runs
#           Tools/Scripts/check_symbols_index.sh; editor_qt_slow runs only the `slow` suite (perf / stress cases);
#           editor_qt_gl runs only the `gl` suite, on the native platform (hidden windows, a real GL
#           context) with XT_REQUIRE_GL=1;
#           import-py = python -m unittest over Tests/import, no build;
#           corpus = Tools/Scripts/corpus_check.sh with the Release xtframe-view / xtframe-pack of this tree, built
#           first unless --no-build (T-401, 43 §43.4; a Debug SW render is ~50x slower, the 4 projects would
#           take hours; strict: a project whose project.json `fonts` are not installed, or a different build, fails, T-426/T-429); the CTest
#           entry corpus_check (which skips such a project) is left out of the ctest suite, which would run it again;
#           ir = Tools/Scripts/check_ir_layout.sh (T-405, 43 §43.5: ir_pod.h sizes and enum limits vs Tests/ir/layout.txt and
#           02 §2.12, the §2.2 minor, the frozen fixture of that minor), no build;
#           roundtrip = the doctest suite `roundtrip` of xt_bundle_tests and xt_editor_service_tests (T-410, 43 §43.2:
#           the frozen cross-version fixtures of Tests/roundtrip read, written back and compared with latest/);
#           shape = Tools/Scripts/check_file_length.sh (must pass) and Tools/Scripts/check_dupcheck.sh (warns when the duplicate
#           group count rises, fails only when dupcheck.py fails) (T-415, 14 §14.5), and Tools/Scripts/check_style_groups.sh
#           (must pass; T-443, 46 §46.4), no build)
# T-439: backends runs xt_backends_tests (Code/Backends' parts) and the CTest entries of Tests/backends and
# Examples/backends (backend_smoke_*, xtframe-view_renderer_*, example_minimal_sdl2_wgpu*); a missing WebGPU
# adapter is a SKIP inside them, not a failure.
# T-437: wgpu runs xt_wgpu_tests (Docs/design/10-backends.md §10.10.3); its "SKIP: no WebGPU adapter" exit
# (0, no doctest run) is a failure here, like a missing GL context, unless XT_ALLOW_NO_GL=1.
# T-267a: XT_REQUIRE_GL=1 is exported for every suite, so `gl` and `editor_qt_gl` FAIL when no GL
# context can be created. Set XT_ALLOW_NO_GL=1 yourself to get the old "no context = skipped" behaviour.
# Each doctest suite prints its summary, every failing test case (name + first failing check, at most
# 30) and, apart, the doctest::may_fail() cases that failed ("known issues", not counted as failures).
# --agent builds with the vs2022-agent preset into build/vs2022-agent instead (= editor + the
# throwaway AI endpoint, Docs/design/20-agent-interface.md 20.1); the `agent` suite needs it.
# Example: Tools/Scripts/dev_check.sh xt_editor_tests xt_editor_qt_tests -- editor editor_qt
# T-361: editor_qt_slow runs its own executable, xt_editor_qt_tests_slow (only the test files that hold
# `slow` cases; Tests/editor_qt/CMakeLists.txt). The suites of PARALLEL_SUITES run at the same time
# (each with its own timeout; their output is buffered and printed in the order asked); the GL, perf,
# slow, agent and ctest suites run one after the other afterwards, so timing-sensitive cases get an
# idle CPU. --changed adds the targets and suites the changes touch (git diff origin/main...HEAD plus
# the work tree; see changed_selection below); --dry-run prints the selection and exits.
#   Tools/Scripts/dev_check.sh --changed            Tools/Scripts/dev_check.sh --changed --dry-run
# Requires XT_QT_DIR (falls back to the dev machine's Qt 5.12 SDK path when that directory exists).
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."
CFG=Debug; BUILD=1; PRESET=vs2022-editor
[ -z "${XT_QT_DIR:-}" ] && [ -d "E:/coding3/CECE_terain_update/Code/SDKs/Qt/5.12.3_pyside2/Qt" ] && export XT_QT_DIR=E:/coding3/CECE_terain_update/Code/SDKs/Qt/5.12.3_pyside2/Qt
targets=(); suites=(); CHANGED=0; DRY_RUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --release) CFG=Release;;
    --no-build) BUILD=0;;
    --agent) PRESET=vs2022-agent;;
    --changed) CHANGED=1;;
    --dry-run) DRY_RUN=1;;
    --) shift; suites=("$@"); break;;
    *) targets+=("$1");;
  esac
  shift
done
# T-361 --changed: the changed files (XT_CHANGED_FILES, one path per line, replaces git for testing)
# mapped to targets and suites, directory by directory. Paths with no entry select nothing; a C++
# path with no entry is named so the caller can add its targets by hand.
changed_files() {
  if [ -n "${XT_CHANGED_FILES:-}" ]; then printf '%s\n' "$XT_CHANGED_FILES"; return; fi
  { git diff --name-only origin/main...HEAD 2>/dev/null; git diff --name-only HEAD 2>/dev/null
    git ls-files --others --exclude-standard 2>/dev/null; } | sort -u
}
changed_selection() {
  local f any=0
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    any=1
    case "$f" in
      Code/Runtime/core/include/xt/ir_pod.h) targets+=(xt_tests); suites+=(core golden ir);;  # T-405
      Code/Runtime/core/*)            targets+=(xt_tests); suites+=(core golden);;
      Code/Runtime/compile/*)         suites+=(core import editor_service);;
      Code/Editor/Import/*)  suites+=(import);;
      Code/Runtime/bundle/*)          suites+=(bundle editor_service roundtrip);;
      Code/Runtime/flow/*)            suites+=(flow editor_qt);;
      Code/Editor/Service/*)  suites+=(editor_service editor_qt);;
      Code/Editor/Qt/*|Code/Editor/App/*) targets+=(xtframe-editor); suites+=(editor_qt);;
      Code/Editor/Core/*)     suites+=(editor editor_service editor_qt);;
      Code/Editor/History/*)  suites+=(history editor_qt);;
      Code/Tools/view/*) suites+=(ctest);;
      Code/Runtime/wgpu/*|Tests/wgpu/*|Tools/CMake/xt_wgpu.cmake) suites+=(wgpu);;  # T-437
      Code/Backends/*|Tests/backends/*|Examples/backends/*|Code/Tools/view/view_renderer*) suites+=(backends);;  # T-439
      Assets/Templates/*|Tests/templates/*) suites+=(templates);;  # T-424
      Tests/editor_qt/*) suites+=(editor_qt);;
      Tests/editor_service/*) suites+=(editor_service);;
      Tests/editor/*)    suites+=(editor);;
      Tests/import/*)    suites+=(import);;
      Tests/bundle/*)    suites+=(bundle);;
      Tests/flow/*)      suites+=(flow);;
      Tests/history/*)   suites+=(history);;
      Tests/golden/*)    suites+=(golden);;
      Tests/corpus/*|Tools/Scripts/corpus_check.*|Tools/Scripts/corpus_fonts.*) suites+=(corpus);;  # T-401, T-429
      Tests/roundtrip/*) suites+=(roundtrip);;  # T-410
      Tests/unit/*)      suites+=(core);;
      Tests/ir/*|Docs/design/02-ir-format.md) suites+=(core ir);;  # T-405
      *.cpp|*.h|*.hpp|*CMakeLists.txt|*.cmake) echo "dev_check --changed: no mapping for $f (add its targets and suites by hand)";;
    esac
  done < <(changed_files)
  # Any change at all: the two no-build checks every task sheet runs.
  [ "$any" = 1 ] && suites+=(headers layering)
  return 0
}
dedupe() { printf '%s\n' "$@" | awk 'NF && !seen[$0]++'; }
[ "$CHANGED" = 1 ] && changed_selection
# Every requested suite builds its own executable(s); ctest needs every executable its entries run
# (the build step deletes every *tests.exe, so one missing here shows up as "Not Run"). T-267d: the
# whole doctest executables `-E '^xt_.*_tests$'` leaves out are not built for it; xt_tests,
# xt_golden and xt_perf are, because that pattern does not match their names.
for s in "${suites[@]}"; do
  case "$s" in
    ctest)     targets+=(xtframe-editor xtframe-view xtframe-pack xtframe-compile xtframe-import xtframe-bake xtframe-gpu-probe xtframe-mcp
                         desktop_app engine_integration lottie_player xt_tests xt_golden xt_perf);;
    editor)    targets+=(xt_editor_tests);;
    editor_service) targets+=(xt_editor_service_tests);;
    editor_qt|editor_qt_gl) targets+=(xt_editor_qt_tests);;
    editor_qt_slow) targets+=(xt_editor_qt_tests_slow);;
    flow)      targets+=(xt_flow_tests);;
    bundle)    targets+=(xt_bundle_tests);;
    history)   targets+=(xt_history_tests);;
    symbols)   targets+=(xt_symbols_tests);;
    templates) targets+=(xt_templates_tests);;  # T-424
    import)    targets+=(xt_import_tests);;
    lottie)    targets+=(xt_lottie_tests);;
    core)      targets+=(xt_tests);;
    golden)    targets+=(xt_golden);;
    gpu)       targets+=(xt_gpu_tests);;
    gl)        targets+=(xt_gl_tests);;
    wgpu)      targets+=(xt_wgpu_tests);;  # T-437
    backends)  targets+=(xt_backends_tests xtframe-view xtframe-pack minimal_sdl2_wgpu);;  # T-439
    text)      targets+=(xt_text_tests);;
    agent)     targets+=(xt_agent_tests);;
    perf)      targets+=(xt_perf);;
    corpus)    targets+=(xtframe-view xtframe-pack);;
    roundtrip) targets+=(xt_bundle_tests xt_editor_service_tests);;  # T-410
  esac
done
# Each suite and target once, in the order first named.
if [ ${#suites[@]} -gt 0 ]; then mapfile -t suites < <(dedupe "${suites[@]}"); fi
if [ ${#targets[@]} -gt 0 ]; then mapfile -t targets < <(dedupe "${targets[@]}"); fi
if [ "$DRY_RUN" = 1 ]; then echo "targets: ${targets[*]}"; echo "suites: ${suites[*]}"; exit 0; fi
# headers, layering, testtemp, import-py, ir and shape compile nothing; when they are all that was asked for, skip the build.
needs_build=0
for s in "${suites[@]}"; do
  case "$s" in headers|layering|testtemp|import-py|ir|shape) ;; *) needs_build=1;; esac
done
[ ${#targets[@]} -eq 0 ] && [ "$needs_build" = 0 ] && BUILD=0

B=build/$PRESET
# T-430 (45 §45.4): the executables of every preset land in bin/<platform>_<arch>/<Config>/ (the
# tests in its tests/); $B keeps the intermediate files, the CTest files and the work directories.
# A non-canonical preset (vs2022-agent: XT_BIN_VARIANT=agent) writes bin/win_x64/<Config>-<variant>/.
BIN_ROOT=bin/win_x64
variant_of() { sed -n 's/^XT_BIN_VARIANT:[A-Z]*=//p' "$B/CMakeCache.txt" 2>/dev/null | tr -d '\r'; }
bin_dir() { local v; v="$(variant_of)"; echo "$BIN_ROOT/$1${v:+-$v}"; }
BIN="$(bin_dir "$CFG")"
# T-267d: one dev_check per build tree. Two runs in the same tree (or a run and its own ctest suite)
# delete and rebuild each other's executables and fail without saying why, so the first one holds
# $B/.dev_check.lock (a directory: mkdir is atomic) with its pid and start time; --no-build runs take
# it too. A second run exits 3 at once while that pid lives; a lock whose pid is gone (a killed run)
# is taken over with a note.
LOCK="$B/.dev_check.lock"
take_lock() { mkdir "$LOCK" 2>/dev/null || return 1; echo "$$" >"$LOCK/pid"; date '+%Y-%m-%d %H:%M:%S' >"$LOCK/started"; }
mkdir -p "$B"
if ! take_lock; then
  lpid="$(cat "$LOCK/pid" 2>/dev/null)"
  # The holder may be between its mkdir and writing the pid.
  [ -z "$lpid" ] && { sleep 1; lpid="$(cat "$LOCK/pid" 2>/dev/null)"; }
  lstart="$(cat "$LOCK/started" 2>/dev/null)"
  if [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null; then
    echo "dev_check is already running in this build tree (pid $lpid, since $lstart)"
    exit 3
  fi
  echo "dev_check: taking over a stale lock in $B (pid ${lpid:-unknown}, since ${lstart:-unknown}, is gone)"
  rm -rf "$LOCK"
  take_lock || { echo "dev_check is already running in this build tree (lock $LOCK taken meanwhile)"; exit 3; }
fi
trap 'rm -rf "$LOCK"' EXIT
trap 'exit 130' INT TERM
fail=0
if [ "$BUILD" = 1 ]; then
  [ -f "$B/CMakeCache.txt" ] || cmake --preset "$PRESET" >/dev/null || { echo "configure FAILED"; exit 2; }
  BIN="$(bin_dir "$CFG")"  # the cache may be new
  find "$BIN/tests" -name "*tests.exe" -delete 2>/dev/null
  rm -f "$BIN/xtframe-editor.exe"
  args=(); for t in "${targets[@]}"; do args+=(--target "$t"); done
  errs="$(cmake --build --preset "$PRESET-$(echo $CFG | tr A-Z a-z)" --parallel "${args[@]}" 2>&1 | grep -E " error |error LNK|warning C|CMake Error|MSB[0-9]+: " | grep -v _deps | head -20)"
  [ -n "$errs" ] && { echo "$errs"; echo "build FAILED"; exit 2; }
  echo "build OK (${targets[*]})"
  # T-401: the corpus suite renders with the Release tools whatever $CFG is.
  case " ${suites[*]} " in
    *" corpus "*) if [ "$CFG" != Release ]; then
                    errs="$(cmake --build --preset "$PRESET-release" --parallel --target xtframe-view --target xtframe-pack 2>&1 |
                            grep -E " error |error LNK|CMake Error|MSB[0-9]+: " | grep -v _deps | head -20)"
                    [ -n "$errs" ] && { echo "$errs"; echo "build FAILED (Release tools for corpus)"; exit 2; }
                    echo "build OK (Release xtframe-view xtframe-pack for corpus)"
                  fi;;
  esac
fi
[ "${XT_ALLOW_NO_GL:-0}" = 1 ] || export XT_REQUIRE_GL=1
# summarize <output>: the doctest / xt_golden summary lines, then every failing doctest case (name and
# its first ERROR line, at most 30), then the may_fail cases that failed, which are not failures.
# Output that is not a doctest case block (xt_golden's [FAIL] lines, a crash message) is shown as is.
summarize() {
  printf '%s\n' "$1" | tr -d '\r' | awk '
    /^===============/ { name = ""; next }
    /^TEST CASE:  / { name = substr($0, 13); if (!(name in seen)) { seen[name] = 1; order[++n] = name } next }
    /Allowed to fail so marking it as not failed/ { if (name != "") mayfail[name] = 1; next }
    /test cases:|Status:|fixtures|fixture\(s\)/ { sum[++ns] = $0; next }
    /ERROR:/ { if (name != "") { if (!(name in first)) first[name] = $0 } else other[++no] = $0; next }
    /\[FAIL\]|FAILED|CRASH/ { if (name == "") other[++no] = $0; next }
    END {
      for (i = 1; i <= ns; i++) print sum[i]
      nf = 0; nk = 0
      for (i = 1; i <= n; i++) { k = order[i]; if (k in mayfail) nk++; else if (k in first) nf++ }
      if (nf > 0) {
        print "failed test cases: " nf; shown = 0
        for (i = 1; i <= n; i++) {
          k = order[i]; if ((k in mayfail) || !(k in first)) continue
          if (++shown > 30) { print "  ... " (nf - 30) " more (" nf " failed in total)"; break }
          print "  " k; print "      " first[k]
        }
      }
      for (i = 1; i <= no && i <= 10; i++) print other[i]
      if (nk > 0) {
        print "known issues (may_fail): " nk
        for (i = 1; i <= n; i++) if (order[i] in mayfail) print "  " order[i]
      }
    }'
}
# run <name> <timeout s> <offscreen 0|1> <exe> [args...]: a non-zero exit code, a missing
# "Status: SUCCESS" / "fixture(s) passed" line or a doctest run of zero cases is a failure.
run() {
  local name="$1" limit="$2" offscreen="$3" exe="$4"; shift 4
  [ -f "$exe" ] || { echo "$name: MISSING $exe"; fail=1; return; }
  local out rc
  if [ "$offscreen" = 1 ]; then out="$(QT_QPA_PLATFORM=offscreen timeout "$limit" "$exe" "$@" 2>&1 </dev/null)"; rc=$?
  else out="$(timeout "$limit" "$exe" "$@" 2>&1 </dev/null)"; rc=$?; fi
  summarize "$out"
  echo "$out" | tr -d '\r' | grep "^xt_wgpu adapter:" | head -1  # T-437: which adapter xt_wgpu_tests used
  # XT_ALLOW_NO_GL=1 only: the GL suites' own "no GL context ... skipping" exit counts as a pass.
  if [ "${XT_ALLOW_NO_GL:-0}" = 1 ] && [ $rc -eq 0 ] && echo "$out" | grep -q "no GL context.*skipping\|skipped: no usable OpenGL"; then
    echo "$name: skipped, no GL context (XT_ALLOW_NO_GL=1)"; return
  fi
  # T-437: likewise xt_wgpu_tests without an adapter (otherwise: no doctest summary = failure).
  if echo "$out" | grep -q "^SKIP: no WebGPU adapter"; then
    echo "$out" | tr -d '\r' | grep "^SKIP: no WebGPU adapter"
    if [ "${XT_ALLOW_NO_GL:-0}" = 1 ] && [ $rc -eq 0 ]; then echo "$name: skipped (XT_ALLOW_NO_GL=1)"; return; fi
  fi
  # 124 is both timeout(1)'s kill and the per-case watchdog of Tests/common/test_watchdog.h (T-267d),
  # which prints the case as "TEST TIMEOUT: ..." first.
  if [ $rc -eq 124 ]; then
    if echo "$out" | grep -q "TEST TIMEOUT:"; then
      echo "$name: TIMEOUT in test case"; echo "$out" | tr -d '\r' | grep "TEST TIMEOUT:"
    else
      echo "$name: TIMEOUT (killed after $limit s)"
    fi
    fail=1; return
  fi
  echo "$out" | grep -q "Status: SUCCESS\|fixture(s) passed" || fail=1
  echo "$out" | grep -q "test cases: *0 |" && { echo "$name: no test case ran"; fail=1; }
  [ $rc -ne 0 ] && { echo "$name: exit code $rc"; fail=1; }
}
# run_suite <suite>: one suite; sets fail=1 on failure (the caller runs it in a subshell).
run_suite() {
  local s="$1"
  echo "=== $s"
  case "$s" in
    editor)    run editor 900 1 "$BIN/tests/xt_editor_tests.exe";;
    editor_service) run editor_service 1800 1 "$BIN/tests/xt_editor_service_tests.exe";;
    editor_qt) run editor_qt 1500 1 "$BIN/tests/xt_editor_qt_tests.exe" --test-suite-exclude=gl,slow
               # Translation check (Tools/Scripts/check_i18n.sh): a warning only, never fails the suite.
               if i18n="$(bash Tools/Scripts/check_i18n.sh 2>&1)"; then echo "$i18n" | tail -2
               else echo "$i18n"; echo "WARNING: untranslated editor strings (Tools/Scripts/check_i18n.sh)"; fi
               # Standard symbol index (T-237): index.json tokens match the symbols' var() uses.
               out="$(bash Tools/Scripts/check_symbols_index.sh 2>&1)" || fail=1; echo "$out" | tail -4;;
    editor_qt_slow) run editor_qt_slow 900 1 "$BIN/tests/xt_editor_qt_tests_slow.exe" --test-suite=slow;;
    # Native platform (no QT_QPA_PLATFORM=offscreen): the cases need a real GL context; their windows stay hidden.
    editor_qt_gl) run editor_qt_gl 600 0 "$BIN/tests/xt_editor_qt_tests.exe" --test-suite=gl;;
    flow)      run flow 900 1 "$BIN/tests/xt_flow_tests.exe";;
    bundle)    run bundle 900 1 "$BIN/tests/xt_bundle_tests.exe";;
    history)   run history 900 1 "$BIN/tests/xt_history_tests.exe";;
    symbols)   run symbols 900 1 "$BIN/tests/xt_symbols_tests.exe";;
    templates) run templates 900 1 "$BIN/tests/xt_templates_tests.exe";;  # T-424
    import)    run import 900 1 "$BIN/tests/xt_import_tests.exe";;
    lottie)    run lottie 900 1 "$BIN/tests/xt_lottie_tests.exe";;
    core)      run core 900 1 "$BIN/tests/xt_tests.exe";;
    golden)    run golden 900 1 "$BIN/tests/xt_golden.exe";;
    gpu)       run gpu 900 1 "$BIN/tests/xt_gpu_tests.exe";;
    text)      run text 900 1 "$BIN/tests/xt_text_tests.exe";;
    gl)        run gl 600 0 "$BIN/tests/xt_gl_tests.exe";;
    wgpu)      run wgpu 600 0 "$BIN/tests/xt_wgpu_tests.exe";;  # T-437
    backends)  run backends 600 0 "$BIN/tests/xt_backends_tests.exe"  # T-439
               out="$(cd "$B" && timeout 900 ctest -C "$CFG" -R '^backend_smoke_|^xtframe-view_renderer_|^example_minimal_sdl2_wgpu' \
                      --no-tests=error 2>&1 </dev/null)"; rc=$?
               echo "$out" | tr -d '\r' | grep -E "tests passed|Failed|\*\*\*" | head -20
               [ $rc -ne 0 ] && { echo "backends: ctest failed (exit $rc)"; fail=1; };;
    agent)     run agent 900 1 "$BIN/tests/xt_agent_tests.exe";;
    perf)      run perf 900 1 "$BIN/tests/xt_perf.exe";;
    # T-410: the cross-version round trips (Tests/roundtrip), in both executables that hold them.
    roundtrip) run roundtrip-bundle 600 1 "$BIN/tests/xt_bundle_tests.exe" --test-suite=roundtrip
               run roundtrip-editor_service 600 1 "$BIN/tests/xt_editor_service_tests.exe" --test-suite=roundtrip;;
    # T-401: the project-level golden corpus; its work tree is $B/corpus-work.
    corpus)    out="$(timeout 1800 bash Tools/Scripts/corpus_check.sh --view "$(bin_dir Release)/xtframe-view.exe" \
                        --pack "$(bin_dir Release)/xtframe-pack.exe" --work "$B/corpus-work" 2>&1 </dev/null)"
               echo "$out" | grep -E "^\[FAIL\]|^CORPUS CHECK|not found|Error" | head -40
               echo "$out" | grep -q "^CORPUS CHECK: PASS" || fail=1;;
    headers)   out="$(bash Tools/Scripts/check_public_headers.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "PUBLIC HEADERS: PASS" || fail=1;;
    layering)  out="$(bash Tools/Scripts/check_layering.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "LAYERING: PASS" || fail=1
               # T-267d: the test temp check runs with layering too; both have to pass.
               out="$(bash Tools/Scripts/check_test_temp.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "TEST TEMP: PASS" || fail=1;;
    testtemp)  out="$(bash Tools/Scripts/check_test_temp.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "TEST TEMP: PASS" || fail=1;;
    ir)        out="$(bash Tools/Scripts/check_ir_layout.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "IR LAYOUT: PASS" || fail=1;;  # T-405
    # T-415: file-length rule (fails) and duplicate-group baseline (warning only).
    shape)     out="$(bash Tools/Scripts/check_file_length.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "FILE LENGTH: PASS" || fail=1
               out="$(bash Tools/Scripts/check_dupcheck.sh 2>&1)"; echo "$out" | tail -6; echo "$out" | grep -q "DUPCHECK: PASS" || fail=1
               out="$(bash Tools/Scripts/check_style_groups.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "style groups check OK" || fail=1;;
    import-py) out="$(NO_COLOR=1 python -m unittest discover -s Tests/import -p "test_*.py" 2>&1)"; echo "$out" | tail -4; { echo "$out" | grep -q "^Ran [0-9]* test" && ! echo "$out" | grep -q "FAILED"; } || fail=1;;
    # T-267d: the command-line tests only (the xt_add_expect_exit_test entries and the other CTest
    # entries that are not a whole doctest executable); the -E pattern leaves out the whole
    # executables (xt_editor_tests, xt_editor_service_tests, xt_editor_qt*_tests, xt_flow_tests,
    # xt_bundle_tests, ..., and xt_tests, xt_golden, xt_perf), which have their own suites above. --no-tests=error: an exclusion that
    # leaves nothing is a failure. A kill shows up as the running test "***Failed" with no output, so
    # the timeout is named explicitly.
    ctest)     out="$(cd "$B" && timeout 1800 ctest -C "$CFG" -E '^xt_(.*_)?tests$|^xt_golden$|^xt_perf$|^corpus_check$' --no-tests=error 2>&1 </dev/null)"; rc=$?
               echo "$out" | grep -E "tests passed|Failed|\*\*\*|Not Run"
               [ $rc -eq 124 ] && echo "ctest: TIMEOUT (killed after 1800 s; the test running then is reported as Failed)"
               echo "$out" | grep -q "100% tests passed" || fail=1;;
    *) echo "unknown suite $s"; fail=1;;
  esac
}
# T-361: the suites below run at the same time; the rest (GL contexts, perf / slow timing cases, the
# agent endpoint and ctest, which runs executables of its own) afterwards, one after the other.
PARALLEL_SUITES=" core import bundle flow editor editor_service editor_qt history symbols templates lottie golden text gpu headers layering testtemp import-py ir roundtrip shape "
OUT="$B/.dev_check_out.$$"; mkdir -p "$OUT"
trap 'rm -rf "$LOCK" "$OUT"; kill $(jobs -p) 2>/dev/null' EXIT
i=0; pids=()
for s in "${suites[@]}"; do
  i=$((i + 1))
  case "$PARALLEL_SUITES" in *" $s "*) ( fail=0; run_suite "$s"; exit $fail ) >"$OUT/$i" 2>&1 & pids[$i]=$!;; esac
done
i=0
for s in "${suites[@]}"; do
  i=$((i + 1))
  [ -n "${pids[$i]:-}" ] && { wait "${pids[$i]}" || fail=1; cat "$OUT/$i"; }
done
i=0
for s in "${suites[@]}"; do
  i=$((i + 1))
  [ -z "${pids[$i]:-}" ] && { ( fail=0; run_suite "$s"; exit $fail ) || fail=1; }
done
echo; [ "$fail" = 0 ] && echo "DEV CHECK: PASS" || echo "DEV CHECK: FAIL"
exit $fail

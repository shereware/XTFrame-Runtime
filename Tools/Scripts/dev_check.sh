#!/usr/bin/env bash
# Developer/agent check for one worktree: build the given targets with the vs2022-editor preset
# (configuring first when needed), delete the test executables so MSBuild relinks them, then run
# each test suite once with a timeout so a hung process never blocks the caller.
#   Tools/Scripts/dev_check.sh [--release] [--no-build] [--agent] [--no-qt512] <target>... -- <suite>...
#   suites: editor | editor_service | editor_qt | editor_qt_slow | editor_qt_gl | flow | bundle | capi | history | net | ai | core | golden |
#           gpu | gl | wgpu | backends | text | agent | import | lottie | symbols | templates | headers | layering | testtemp | import-py | scripts-py | ctest | corpus | ir |
#           roundtrip | shape | skills | qt512 | qt512-all | wasm
#          (ctest = ctest -E '^xt_.*_tests$', the command-line tests (T-267d); headers = Tools/Scripts/check_public_headers.sh, no build;
#           layering = Tools/Scripts/check_layering.sh (T-274, R never includes / links E) and, since T-267d,
#           Tools/Scripts/check_test_temp.sh too (both must pass), no build;
#           testtemp = Tools/Scripts/check_test_temp.sh alone (T-267d, no test uses the system temp directory
#           directly), no build;
#           editor_qt runs xt_editor_qt_tests without the doctest suites `gl` and `slow`, in XT_EDITOR_QT_SHARDS
#           processes at once (T-838; default 4, 1 = one process; see run_editor_qt) (T-361: editor_qt_slow
#           runs the separate xt_editor_qt_tests_slow, built from the test files holding `slow` cases), and also runs
#           Tools/Scripts/check_symbols_index.sh; editor_qt_slow runs only the `slow` suite (perf / stress cases);
#           editor_qt_gl runs only the `gl` suite, on the native platform (hidden windows, a real GL
#           context) with XT_REQUIRE_GL=1;
#           import-py = python -m unittest over Tests/import, no build;
#           scripts-py = python -m unittest over Tests/scripts (T-751: perf_agg / perf_run / perf_report), no build;
#           corpus = Tools/Scripts/corpus_check.sh with the Release xtframe-view / xtframe-pack of this tree, built
#           first unless --no-build (T-401, 43 §43.4; a Debug SW render is ~50x slower, the 4 projects would
#           take hours; strict: a project whose project.json `fonts` are not installed, or a different build, fails, T-426/T-429); the CTest
#           entry corpus_check (which skips such a project) is left out of the ctest suite, which would run it again;
#           ir = Tools/Scripts/check_ir_layout.sh (T-405, 43 §43.5: ir_pod.h sizes and enum limits vs Tests/ir/layout.txt and
#           02 §2.12, the §2.2 minor, the frozen fixture of that minor), no build;
#           capi = xt_capi_tests and xt_capi_c_compile (T-504, 50 §50.4: the C ABI shared library xtframe and
#           its header compiled as C99);
#           T-552: capi / net / ai are a SKIP, not a failure, when the build tree has the option off (capi:
#           XT_BUILD_CAPI; net, ai: XT_EDITOR_AI with XT_BUILD_EDITOR or XT_BUILD_EDITOR_CLI); xtframe-service
#           (and ai's ai_replay_* entries) only with XT_BUILD_EDITOR_CLI;
#           roundtrip = the doctest suite `roundtrip` of xt_bundle_tests and xt_editor_service_tests (T-410, 43 §43.2:
#           the frozen cross-version fixtures of Tests/roundtrip read, written back and compared with latest/);
#           shape = Tools/Scripts/check_file_length.sh (must pass) and Tools/Scripts/check_dupcheck.sh (warns when the duplicate
#           group count rises, fails only when dupcheck.py fails) (T-415, 14 §14.5), and Tools/Scripts/check_style_groups.sh
#           (must pass; T-443, 46 §46.4) and Tools/Scripts/check_effect_catalog.py (must pass; T-463, 46 §46.13.4), no build;
#           skills = Tools/Scripts/sync_skill_mirror.sh --check (T-485: .claude/skills/{xtui,xtui-editor} must equal
#           Skills/skills/ byte for byte; --changed adds it for every change) and Tools/Scripts/check_skill_extras.sh
#           (T-550: the built-in agent's reference copies, tokens.json == xtframe-symbols tokens-json) and
#           Tools/Scripts/check_generated_tables.sh (T-834: gen_property_table.py's C++ outputs == the committed files), no build;
#           qt512 = Tools/Scripts/qt512_syntax_check.sh over the Code/Editor .cpp files changed or added relative to
#           origin/main (git diff origin/main...HEAD plus the work tree; XT_CHANGED_FILES replaces git), flags from this
#           tree's xt_clang_check lists, against the Qt 5.12 headers (T-470, 17 §17.9; no Qt 5.12 or no clang++: SKIP);
#           qt512-all = the same over every Code/Editor Qt source of the tree; both no build, but the tree must be configured;
#           wasm = Tools/Scripts/build_wasm.sh then Tools/Scripts/check_wasm.sh (T-579, T-520, 52 §52.5, Experimental: the Node.js builds of the
#           command-line tools against this machine's native Release ones); needs emcmake (Emscripten SDK on PATH, EMSDK or
#           EMSCRIPTEN_ROOT) and builds its own tree, not the preset's; without emcmake: SKIPPED, not a failure)
# T-692 checklist: changes to effects, shadows, image sampling or painter output: also run `perf` and
# `symbols` (xt_perf's effect workloads and zero-allocation cases; the symbol library's SW previews).
# --changed does not select them for Code/Runtime changes, and both caught regressions the usual gate missed.
# T-470: qt512 is added by itself whenever editor_qt is asked for or xtframe-editor is a target (--no-qt512: not).
# T-517: net runs xt_net_tests; ai runs xt_ai_tests and the CTest entries ai_replay_* (xtframe-editor --service
# --agent on a replay script, Docs/design/51-builtin-ai.md §51.7), so it builds xtframe-editor too.
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
# Qt: XT_QT_DIR, else the newest Qt 5.15, then 5.12, under Code/SDKs/Qt (T-442; Code/SDKs/Qt/README.md),
# else the same under the main worktree when this is a git worktree (T-852).
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."
CFG=Debug; BUILD=1; PRESET=vs2022-editor
# T-442 / T-852: XT_QT_DIR (and XT_QT512_DIR), when unset, from Code/SDKs/Qt of this checkout, else of
# the main worktree (Tools/Scripts/xt_qt_env.sh; same rule as Tools/CMake/xt_qt.cmake).
. Tools/Scripts/xt_qt_env.sh
xt_qt_resolve
targets=(); suites=(); CHANGED=0; DRY_RUN=0; NO_QT512=0; NO_BUILD_ARG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --release) CFG=Release;;
    --no-build) BUILD=0; NO_BUILD_ARG=1;;
    --agent) PRESET=vs2022-agent;;
    --changed) CHANGED=1;;
    --dry-run) DRY_RUN=1;;
    --no-qt512) NO_QT512=1;;
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
      # 2026-11-20: text shaping and line layout move the symbol library's reference images too
      # (T-801 changed one and never ran that suite). More specific than core/*, so it comes first.
      Code/Runtime/text/*|Tests/text/*|Code/Runtime/core/src/text*|Code/Runtime/core/src/layout_text*)
                                      targets+=(xt_tests); suites+=(core golden text symbols);;
      Code/Runtime/core/*)            targets+=(xt_tests); suites+=(core golden);;
      Code/Runtime/sw/*)              targets+=(xt_tests); suites+=(core golden corpus);;  # corpus: T-855
      Code/Runtime/gpu/*|Tests/gpu/*) suites+=(gpu gl wgpu golden);;
      Code/Runtime/gl/*|Tests/gl/*)   suites+=(gl);;
      Tests/common/*)                 suites+=(core gpu gl wgpu);;
      Code/Runtime/compile/*)         suites+=(core import editor_service);;
      Code/Editor/Import/*)  suites+=(import);;
      # capi: its tests pack bundles through xtframe-pack (the T-714 publish default broke one and
      # only the WSL leg of the version gate saw it).
      Code/Runtime/bundle/*)          suites+=(bundle editor_service roundtrip capi);;
      Code/Tools/pack/*)              suites+=(bundle capi ctest corpus);;  # corpus: T-855
      VERSION.txt)                    suites+=(corpus);;  # T-855: the compiler version is in every .xtir, so in every manifest hash
      Code/Tools/import/*)            suites+=(import ctest);;
      Code/Tools/symbols/*|Assets/Symbols/*|Tests/symbols/*) suites+=(symbols);;
      Code/Runtime/flow/*)            suites+=(flow editor_qt);;
      Code/Runtime/capi/*|Tests/capi/*) suites+=(capi);;  # T-504
      # ai: the tool host counts the service's operations per scenario (three new timeline ops
      # went uncounted until the version gate).
      Code/Editor/Service/*)  suites+=(editor_service editor_qt ai);;
      Code/Editor/Contracts/*|Assets/Schemas/*) suites+=(editor_service ai);;
      Code/Editor/Qt/*|Code/Editor/App/*) targets+=(xtframe-editor); suites+=(editor_qt);;
      Code/Editor/Core/*)     suites+=(editor editor_service editor_qt);;
      Code/Editor/History/*)  suites+=(history editor_qt);;
      Code/Editor/Net/*)      suites+=(net ai);;  # T-517: xt_ai sits on xt_net
      Code/Editor/AI/*)       suites+=(ai editor_qt);;  # T-517: the AI panel's mode (c) too
      Code/Editor/Cli/*) targets+=(xtframe-service); suites+=(editor_service ai);;  # T-518: --service, --agent
      Code/Tools/view/*) suites+=(ctest);;
      Code/Tools/wasm/*|Tools/Scripts/*wasm*|Tools/CMake/xt_wasm.cmake) suites+=(wasm);;  # T-579 (T-520)
      Code/Runtime/wgpu/*|Tests/wgpu/*|Tools/CMake/xt_wgpu.cmake) suites+=(wgpu);;  # T-437
      Code/Backends/*|Tests/backends/*|Examples/backends/*|Code/Tools/view/view_renderer*) suites+=(backends);;  # T-439
      Assets/Templates/*|Tests/templates/*) suites+=(templates);;  # T-424
      Skills/*|.claude/skills/*|Tools/Scripts/sync_skill_mirror.sh|Tools/Scripts/check_skill_extras.sh) suites+=(skills);;  # T-485, T-550
      Tests/editor_qt/*) suites+=(editor_qt);;
      Tests/editor_service/*) suites+=(editor_service);;
      Tests/editor/*)    suites+=(editor);;
      Tests/import/*)    suites+=(import);;
      Tests/scripts/*|Tools/Scripts/perf_*.py|Tools/Scripts/bench_war_table*.py) suites+=(scripts-py);;  # T-751
      Tests/bundle/*)    suites+=(bundle);;
      Tests/flow/*)      suites+=(flow);;
      Tests/history/*)   suites+=(history);;
      Tests/net/*)       suites+=(net);;  # T-517
      Tests/ai/*)        suites+=(ai);;   # T-517
      Tests/golden/*)    suites+=(golden);;
      Tests/corpus/*|Tools/Scripts/corpus_check.*|Tools/Scripts/corpus_fonts.*) suites+=(corpus);;  # T-401, T-429
      Tests/roundtrip/*) suites+=(roundtrip);;  # T-410
      Tests/unit/*)      suites+=(core);;
      Tests/ir/*|Docs/design/02-ir-format.md) suites+=(core ir);;  # T-405
      *.cpp|*.h|*.hpp|*CMakeLists.txt|*.cmake) echo "dev_check --changed: no mapping for $f (add its targets and suites by hand)";;
    esac
  done < <(changed_files)
  # Any change at all: the no-build checks every task sheet runs (T-485: skills, the skill mirror).
  [ "$any" = 1 ] && suites+=(headers layering skills)
  return 0
}
dedupe() { printf '%s\n' "$@" | awk 'NF && !seen[$0]++'; }
[ "$CHANGED" = 1 ] && changed_selection
# T-470: the Qt 5.12 headers leg rides along with the editor's Qt code (MSVC on Qt 5.15 misses what 5.12 needs).
if [ "$NO_QT512" = 0 ]; then
  case " ${suites[*]} ${targets[*]} " in *" editor_qt "*|*" xtframe-editor "*) suites+=(qt512);; esac
fi
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
    flow)      targets+=(xt_flow_tests xt_flow_alloc_tests);;
    bundle)    targets+=(xt_bundle_tests);;
    capi)      targets+=(xt_capi_tests xt_capi_c_compile);;  # T-504
    history)   targets+=(xt_history_tests);;
    net)       targets+=(xt_net_tests);;  # T-517
    ai)        targets+=(xt_ai_tests xtframe-service);;  # T-518: xtframe-service for ai_replay_*
    symbols)   targets+=(xt_symbols_tests xtframe-symbols);;  # T-499: + the lint of the preset library
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
# headers, layering, testtemp, import-py, scripts-py, ir, shape, skills, qt512, qt512-all and wasm (its own Emscripten tree) need no preset build; when they are all that was asked for, skip the build.
needs_build=0
for s in "${suites[@]}"; do
  case "$s" in headers|layering|testtemp|import-py|scripts-py|ir|shape|skills|qt512|qt512-all|wasm) ;; *) needs_build=1;; esac
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
  if [ ! -f "$B/CMakeCache.txt" ]; then
    # T-799: both presets this script uses (vs2022-editor, vs2022-agent) pass $env{XT_QT_DIR} to CMake;
    # without it (and no Qt under Code/SDKs/Qt, above) the configure fails half way.
    if [ -z "${XT_QT_DIR:-}" ]; then
      echo "dev_check: XT_QT_DIR is not set and Code/SDKs/Qt (here or in the main worktree) has no Qt 5.15 / 5.12; export XT_QT_DIR (preset $PRESET needs it) and rerun"
      exit 2
    fi
    # T-799: a failed configure leaves a partial CMakeCache.txt, and the next run would skip the
    # configure and fail in MSBuild (MSB1009). Delete that one file only ($B or build/ may be a junction).
    if ! cmake --preset "$PRESET" >/dev/null; then
      rm -f "$B/CMakeCache.txt"
      echo "configure FAILED (preset $PRESET; $B/CMakeCache.txt removed so the next run configures again)"
      exit 2
    fi
  fi
  BIN="$(bin_dir "$CFG")"  # the cache may be new
  # 2026-11-20: a suite that was asked for is built, not skipped. XT_BUILD_CAPI defaults to OFF in
  # the presets, so the capi suite used to be skipped in every task tree and its tests were first
  # run by the version gate's WSL leg. Turning the option on in this tree's cache is a reconfigure
  # of one option; it stays on for later runs.
  case " ${suites[*]} " in
    *" capi "*) if ! sed -n 's/^XT_BUILD_CAPI:BOOL=//p' "$B/CMakeCache.txt" 2>/dev/null | tr -d '\r' | grep -qiE '^(ON|TRUE|1|YES)$'; then
                  if cmake --preset "$PRESET" -DXT_BUILD_CAPI=ON >/dev/null; then echo "dev_check: XT_BUILD_CAPI turned ON in $B (the capi suite was asked for)"
                  else echo "dev_check: could not turn XT_BUILD_CAPI on in $B; the capi suite will be skipped"; fi
                fi;;
  esac
fi
# T-552: options that default to OFF (XT_BUILD_CAPI, XT_BUILD_EDITOR_CLI; 2026-10-31 spec review 7.5) or that a
# tree turns off (XT_EDITOR_AI): the suites and targets that need them are SKIPped, not failed.
cache_on() { sed -n "s/^$1:BOOL=//p" "$B/CMakeCache.txt" 2>/dev/null | tr -d '\r' | grep -qiE '^(ON|TRUE|1|YES)$'; }
SKIPPED=()
if [ -f "$B/CMakeCache.txt" ]; then
  editor_layers=0; { cache_on XT_BUILD_EDITOR || cache_on XT_BUILD_EDITOR_CLI; } && editor_layers=1
  ai_on=0; [ "$editor_layers" = 1 ] && cache_on XT_EDITOR_AI && ai_on=1
  kept=()
  for s in "${suites[@]}"; do
    case "$s" in
      capi) cache_on XT_BUILD_CAPI || { SKIPPED+=("capi: SKIP (XT_BUILD_CAPI is OFF in $B)"); continue; };;
      net|ai) [ "$ai_on" = 1 ] || { SKIPPED+=("$s: SKIP (XT_EDITOR_AI or the editor layers are OFF in $B)"); continue; };;
    esac
    kept+=("$s")
  done
  suites=("${kept[@]}")
  kept=()
  for t in "${targets[@]}"; do
    case "$t" in
      xt_capi_tests|xt_capi_c_compile) cache_on XT_BUILD_CAPI || continue;;
      xt_net_tests|xt_ai_tests) [ "$ai_on" = 1 ] || continue;;
      xtframe-service) cache_on XT_BUILD_EDITOR_CLI || { SKIPPED+=("xtframe-service: SKIP (XT_BUILD_EDITOR_CLI is OFF in $B)"); continue; };;
    esac
    kept+=("$t")
  done
  targets=("${kept[@]}")
fi
for m in "${SKIPPED[@]}"; do echo "$m"; done
if [ "$BUILD" = 1 ] && [ ${#targets[@]} -eq 0 ]; then
  BUILD=0  # everything asked for was skipped: never fall through to a build of every target
fi
if [ "$BUILD" = 1 ]; then
  find "$BIN/tests" -name "*tests.exe" -delete 2>/dev/null
  # T-486: only when the editor is rebuilt here; otherwise a check of other targets would leave
  # the build tree without an editor.
  case " ${targets[*]} " in *" xtframe-editor "*) rm -f "$BIN/xtframe-editor.exe";; esac
  # 2026-11-21 (T-817): the ctest suite also runs entries whose executables are not in its fixed
  # list (the backend tests and smokes, the intro hosts, the bench and probe tools). They used to
  # pass only when an earlier build had left them in the tree, and xt_backends_tests never could
  # (deleted two lines up). They are added when the configured tree has them (some are optional).
  case " ${suites[*]} " in *" ctest "*)
    for t in xt_backends_tests minimal_sdl2_wgpu xtframe-design xtframe-symbols xtframe-bench xt_gpu_mem_probe              intro_headless_test intro_cpp_host intro_capi_host; do
      [ -n "$(find "$B" -name "$t.vcxproj" -print -quit 2>/dev/null)" ] && targets+=("$t")
    done
    while IFS= read -r v; do targets+=("$(basename "$v" .vcxproj)"); done       < <(find "$B" -name 'xt_backend_smoke_*.vcxproj' 2>/dev/null);;
  esac
  args=(); for t in "${targets[@]}"; do args+=(--target "$t"); done
  # XT_BUILD_JOBS=<n> caps the number of projects built at once (two task trees share this machine);
  # unset = the generator's default. The compiler's own /MP is capped separately with _CL_=-MP<n>.
  PAR=(--parallel); [ -n "${XT_BUILD_JOBS:-}" ] && PAR=(--parallel "$XT_BUILD_JOBS")
  errs="$(cmake --build --preset "$PRESET-$(echo $CFG | tr A-Z a-z)" "${PAR[@]}" "${args[@]}" 2>&1 | grep -E " error |error LNK|warning C|CMake Error|error MSB[0-9]+: " | grep -v _deps | head -20)"
  [ -n "$errs" ] && { echo "$errs"; echo "build FAILED"; exit 2; }
  echo "build OK (${targets[*]})"
  # T-401: the corpus suite renders with the Release tools whatever $CFG is.
  case " ${suites[*]} " in
    *" corpus "*) if [ "$CFG" != Release ]; then
                    errs="$(cmake --build --preset "$PRESET-release" --parallel --target xtframe-view --target xtframe-pack 2>&1 |
                            grep -E " error |error LNK|CMake Error|error MSB[0-9]+: " | grep -v _deps | head -20)"
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
# T-838: editor_qt runs in XT_EDITOR_QT_SHARDS processes at once (default 4; 1 = the single process of
# before, with T-810's 2874 s limit). Measured 2026-11-21 (Debug, 974 cases): one process 707 s.
# A case belongs to the shard of its source file, hash("<salt>:<file name>") mod N, so it stays in its
# shard while the file keeps its name (index ranges, --first/--last, move every case when one is added);
# salt 41 puts 2026-11-21's per-file times for N=4 at 174 / 180 / 182 / 152 s. The files of
# EDITOR_QT_SERIAL_FILES (they decode the fixture video through Qt Multimedia, which misses its first
# frame or blocks when the CPU is saturated: 1 in 10 beside three busy test processes, 3 in 40 with
# four decoding processes, measured 2026-11-21; T-610) run afterwards in one more process, alone.
# Each process gets its own TMP / TEMP / APPDATA / LOCALAPPDATA / XDG_* directories under
# $B/editor_qt_shards/<k>: the product code writes service.json and the logs under LOCALAPPDATA, the
# symbol user library under APPDATA, the bundle browser's packs, the crash log and its scratch
# directories under the temp directory (fixed names; T-838 sheet, step 1). Each has its own 1500 s
# timeout and keeps its output in $B/editor_qt_shards/<k>/out.txt. A failing shard fails the suite,
# and the cases run must equal, shard by shard and in total, what --list-test-cases counts for the
# unsharded run (no case falls between two shards).
EDITOR_QT_SHARD_SALT=41
EDITOR_QT_SERIAL_FILES="test_video_frame_source.cpp test_preview_capture.cpp test_reference_viewer.cpp test_media_preview.cpp"
# editor_qt_shard_start <exe> <key> <files>: one shard in the background (its pid in $shard_pid).
editor_qt_shard_start() {
  local exe="$1" key="$2" files="$3" d w
  d="$B/editor_qt_shards/$key"; mkdir -p "$d/tmp" "$d/appdata" "$d/localappdata" "$d/xdg"
  w="$(cd "$d" && { pwd -W 2>/dev/null || pwd; })"
  printf '%s\n' "$files" >"$d/files"
  ( s0=$SECONDS
    TMP="$w/tmp" TEMP="$w/tmp" TMPDIR="$w/tmp" APPDATA="$w/appdata" LOCALAPPDATA="$w/localappdata" \
      XDG_CONFIG_HOME="$w/xdg/config" XDG_DATA_HOME="$w/xdg/data" XDG_CACHE_HOME="$w/xdg/cache" \
      XDG_STATE_HOME="$w/xdg/state" QT_QPA_PLATFORM=offscreen \
      timeout 1500 "$exe" --test-suite-exclude=gl,slow "--source-file=$files" >"$d/out.txt" 2>&1 </dev/null
    echo "$? $((SECONDS - s0))" >"$d/rc" ) &
  shard_pid=$!
}
# editor_qt_shard_report <exe> <key> <label> <expected cases> <files count>: waits for the shard,
# prints its summary, adds its counts to sum / psum / fsum and sets fail=1 when it failed.
editor_qt_shard_report() {
  local exe="$1" key="$2" label="$3" exp="$4" nfiles="$5" d out rc=1 secs=? ran passed failed bad=0
  d="$B/editor_qt_shards/$key"
  read -r rc secs <"$d/rc"
  out="$(tr -d '\r' <"$d/out.txt")"
  echo "--- editor_qt $label: $nfiles files, $exp cases, $secs s ($d/out.txt)"
  summarize "$out"
  read -r ran passed failed < <(printf '%s\n' "$out" |
    sed -n 's/^\[doctest\] test cases: *\([0-9]*\) | *\([0-9]*\) passed | *\([0-9]*\) failed.*/\1 \2 \3/p' | tail -1)
  ran="${ran:-0}"; sum=$((sum + ran)); psum=$((psum + ${passed:-0})); fsum=$((fsum + ${failed:-0}))
  if [ "$rc" = 124 ]; then
    if echo "$out" | grep -q "TEST TIMEOUT:"; then echo "editor_qt $label: TIMEOUT in test case"; echo "$out" | grep "TEST TIMEOUT:"
    else echo "editor_qt $label: TIMEOUT (killed after 1500 s)"; fi
    bad=1
  fi
  echo "$out" | grep -q "Status: SUCCESS" || bad=1
  [ "$rc" != 0 ] && [ "$rc" != 124 ] && { echo "editor_qt $label: exit code $rc"; bad=1; }
  [ "$ran" != "$exp" ] && { echo "editor_qt $label: ran $ran cases, its files hold $exp"; bad=1; }
  [ "$bad" = 0 ] && return 0
  fail=1
  echo "  rerun the shard: TMP=$d/tmp TEMP=$d/tmp QT_QPA_PLATFORM=offscreen $exe --test-suite-exclude=gl,slow --source-file=$(cat "$d/files")"
  # One rerun line per failed case (doctest filters: "\" and "," escaped with a backslash).
  printf '%s\n' "$out" | awk '
    /^===============/ { name = ""; next }
    /^TEST CASE:  / { name = substr($0, 13); next }
    /Allowed to fail so marking it as not failed/ { if (name != "") mayfail[name] = 1; next }
    /ERROR:/ { if (name != "" && !(name in seen)) { seen[name] = 1; order[++n] = name } }
    END { for (i = 1; i <= n && i <= 30; i++) if (!(order[i] in mayfail)) print order[i] }' |
  while IFS= read -r name; do
    name="${name//\\/\\\\}"; name="${name//,/\\,}"; name="${name//\"/\\\"}"
    echo "  rerun the case: QT_QPA_PLATFORM=offscreen $exe --test-suite-exclude=gl,slow \"--test-case=$name\""
  done
  return 0
}
run_editor_qt() {
  local exe="$1" n="${XT_EDITOR_QT_SHARDS:-4}" filter=--test-suite-exclude=gl,slow
  case "$n" in ''|*[!0-9]*) n=0;; esac
  if [ "$n" -lt 1 ]; then echo "editor_qt: XT_EDITOR_QT_SHARDS must be a positive integer (got '${XT_EDITOR_QT_SHARDS:-}')"; fail=1; return; fi
  if [ "$n" = 1 ]; then run editor_qt 2874 1 "$exe" "$filter"; return; fi
  [ -f "$exe" ] || { echo "editor_qt: MISSING $exe"; fail=1; return; }
  local list plan total
  rm -rf "$B/editor_qt_shards"; mkdir -p "$B/editor_qt_shards"
  list="$(QT_QPA_PLATFORM=offscreen timeout 300 "$exe" "$filter" --list-test-cases --reporters=xml 2>&1 </dev/null)"
  # One line "<shard 0..N-1 or serial> <file name> <cases>" per source file, then "total <cases>".
  plan="$(printf '%s\n' "$list" | tr -d '\r' | awk -v n="$n" -v salt="$EDITOR_QT_SHARD_SALT" -v serial="$EDITOR_QT_SERIAL_FILES" '
    BEGIN { for (i = 32; i < 127; i++) ord[sprintf("%c", i)] = i
            ns = split(serial, sl, " "); for (i = 1; i <= ns; i++) alone[sl[i]] = 1 }
    function hash(s,  v, i) { v = 0; for (i = 1; i <= length(s); i++) v = (v * 131 + ord[substr(s, i, 1)]) % 2147483647; return v }
    /<TestCase / { if (!match($0, /filename="[^"]*"/)) next
                   f = substr($0, RSTART + 10, RLENGTH - 11); sub(/.*[\\\/]/, "", f)
                   if (!(f in c)) order[++nf] = f
                   c[f]++; total++ }
    END { for (i = 1; i <= nf; i++) print ((order[i] in alone) ? "serial" : hash(salt ":" order[i]) % n), order[i], c[order[i]]
          print "total", total + 0 }')"
  total="$(printf '%s\n' "$plan" | awk '$1 == "total" { print $2 }')"
  if [ "${total:-0}" -eq 0 ]; then
    echo "editor_qt: listing the test cases found none"; printf '%s\n' "$list" | tail -5; fail=1; return
  fi
  # shard_files <key> / shard_cases <key> / shard_count <key>: from the plan.
  shard_files() { printf '%s\n' "$plan" | awk -v k="$1" '$1 == k { printf "%s*%s", (s++ ? "," : ""), $2 }'; }
  shard_cases() { printf '%s\n' "$plan" | awk -v k="$1" '$1 == k { s += $3 } END { print s + 0 }'; }
  shard_count() { printf '%s\n' "$plan" | awk -v k="$1" '$1 == k { s++ } END { print s + 0 }'; }
  local k files t0=$SECONDS shard_pid pids=() sum=0 psum=0 fsum=0
  for ((k = 0; k < n; k++)); do
    files="$(shard_files "$k")"
    [ -z "$files" ] && continue
    editor_qt_shard_start "$exe" "$k" "$files"; pids[$k]=$shard_pid
  done
  for ((k = 0; k < n; k++)); do
    [ -n "${pids[$k]:-}" ] || continue
    wait "${pids[$k]}"
    editor_qt_shard_report "$exe" "$k" "shard $((k + 1))/$n" "$(shard_cases "$k")" "$(shard_count "$k")"
  done
  files="$(shard_files serial)"
  if [ -n "$files" ]; then
    editor_qt_shard_start "$exe" serial "$files"; wait "$shard_pid"
    editor_qt_shard_report "$exe" serial "serial shard (Qt Multimedia, alone)" "$(shard_cases serial)" "$(shard_count serial)"
  fi
  echo "editor_qt (all shards): test cases: $sum | $psum passed | $fsum failed; unsharded count $total; wall $((SECONDS - t0)) s"
  [ "$sum" != "$total" ] && { echo "editor_qt: the shards ran $sum cases, the unsharded run has $total"; fail=1; }
  return 0
}
# run_suite <suite>: one suite; sets fail=1 on failure (the caller runs it in a subshell).
run_suite() {
  local s="$1"
  echo "=== $s"
  case "$s" in
    editor)    run editor 900 1 "$BIN/tests/xt_editor_tests.exe";;
    editor_service) run editor_service 1800 1 "$BIN/tests/xt_editor_service_tests.exe";;
    # T-810: twice a standalone run of the whole suite (measured 2026-11-20: 1437 s for 961 cases, Debug,
    # other builds on the machine); 1500 s was hit twice while other builds ran (T-714).
    editor_qt) run_editor_qt "$BIN/tests/xt_editor_qt_tests.exe"  # T-838: in shards (see run_editor_qt)
               # Translation check (Tools/Scripts/check_i18n.sh): a warning only, never fails the suite.
               if i18n="$(bash Tools/Scripts/check_i18n.sh 2>&1)"; then echo "$i18n" | tail -2
               else echo "$i18n"; echo "WARNING: untranslated editor strings (Tools/Scripts/check_i18n.sh)"; fi
               # Standard symbol index (T-237): index.json tokens match the symbols' var() uses.
               out="$(bash Tools/Scripts/check_symbols_index.sh 2>&1)" || fail=1; echo "$out" | tail -4;;
    editor_qt_slow) run editor_qt_slow 900 1 "$BIN/tests/xt_editor_qt_tests_slow.exe" --test-suite=slow;;
    # Native platform (no QT_QPA_PLATFORM=offscreen): the cases need a real GL context; their windows stay hidden.
    editor_qt_gl) run editor_qt_gl 600 0 "$BIN/tests/xt_editor_qt_tests.exe" --test-suite=gl;;
    flow)      run flow 900 1 "$BIN/tests/xt_flow_tests.exe"
               run flow_alloc 300 1 "$BIN/tests/xt_flow_alloc_tests.exe";;  # T-476
    bundle)    run bundle 900 1 "$BIN/tests/xt_bundle_tests.exe";;
    # T-504: the C ABI (doctest), then the C99 compile check (no doctest summary: its exit code alone).
    # T-799: 1500 s, not 600: the whole Debug suite runs past 600 s (measured 2026-11-19: ~683 s for 91 cases;
    # one case, `capi state 8`, ~173 s alone).
    capi)      run capi 1500 1 "$BIN/tests/xt_capi_tests.exe"
               if [ -f "$BIN/tests/xt_capi_c_compile.exe" ]; then
                 out="$(timeout 60 "$BIN/tests/xt_capi_c_compile.exe" 2>&1 </dev/null)"; rc=$?; echo "$out" | tail -2
                 [ $rc -ne 0 ] && { echo "capi_c_compile: exit code $rc"; fail=1; }
               else echo "capi_c_compile: MISSING $BIN/tests/xt_capi_c_compile.exe"; fail=1; fi;;
    history)   run history 900 1 "$BIN/tests/xt_history_tests.exe";;
    net)       run net 600 1 "$BIN/tests/xt_net_tests.exe";;  # T-517
    # T-517: the agent loop suite, then the headless replay runs (ai_replay_<scenario>).
    ai)        run ai 900 1 "$BIN/tests/xt_ai_tests.exe"
               # T-552: the replay entries run xtframe-service, which only an XT_BUILD_EDITOR_CLI tree has.
               if ! cache_on XT_BUILD_EDITOR_CLI; then echo "ai: ai_replay_* SKIP (XT_BUILD_EDITOR_CLI is OFF in $B)"
               else
               out="$(cd "$B" && QT_QPA_PLATFORM=offscreen timeout 600 ctest -C "$CFG" -R '^ai_replay_' --no-tests=error 2>&1 </dev/null)"; rc=$?
               echo "$out" | tr -d '\r' | grep -E "tests passed|Failed|\*\*\*|Not Run" | head -20
               [ $rc -ne 0 ] && { echo "ai: ctest ai_replay_* failed (exit $rc)"; fail=1; }
               fi;;
    symbols)   run symbols 900 1 "$BIN/tests/xt_symbols_tests.exe"
               # T-499 (49 §49.6): the preset library lints clean (errors fail; warnings are shown).
               if [ -f "$BIN/xtframe-symbols.exe" ]; then
                 out="$(timeout 300 "$BIN/xtframe-symbols.exe" lint Assets/Symbols 2>&1 </dev/null)" || { fail=1; echo "$out" | tail -20; }
                 echo "symbols lint: $(echo "$out" | tail -1)"
               else echo "symbols: MISSING $BIN/xtframe-symbols.exe"; fail=1; fi;;
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
               out="$(bash Tools/Scripts/check_style_groups.sh 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "style groups check OK" || fail=1
               out="$(python Tools/Scripts/check_effect_catalog.py 2>&1)"; echo "$out" | tail -12; echo "$out" | grep -q "effect catalog check OK" || fail=1;;
    # T-485: the skill mirror (.claude/skills/{xtui,xtui-editor} = Skills/skills/).
    skills)    out="$(bash Tools/Scripts/sync_skill_mirror.sh --check 2>&1)"; echo "$out" | tail -20; echo "$out" | grep -q "SKILL MIRROR: PASS" || fail=1
               bash Tools/Scripts/check_skill_extras.sh || fail=1  # T-550: AI reference copies, tokens-json
               # T-834: the four C++ outputs of gen_property_table.py equal the committed files.
               out="$(bash Tools/Scripts/check_generated_tables.sh 2>&1)"; echo "$out" | tail -8; echo "$out" | grep -q "GENERATED TABLES: PASS" || fail=1;;
    # T-470: the Qt 5.12 headers syntax check (Tools/Scripts/qt512_syntax_check.sh), changed files / all files.
    qt512)     q="$(changed_files | grep -E '^Code/Editor/.*\.cpp$')"
               if [ -z "$q" ]; then echo "qt512: nothing to check"
               else out="$(printf '%s\n' "$q" | bash Tools/Scripts/qt512_syntax_check.sh --tree "$B" 2>&1)" || fail=1; echo "$out" | tail -40; fi;;
    qt512-all) out="$(bash Tools/Scripts/qt512_syntax_check.sh --tree "$B" --all 2>&1)" || fail=1; echo "$out" | tail -40;;
    import-py) out="$(NO_COLOR=1 python -m unittest discover -s Tests/import -p "test_*.py" 2>&1)"; echo "$out" | tail -4; { echo "$out" | grep -q "^Ran [0-9]* test" && ! echo "$out" | grep -q "FAILED"; } || fail=1;;
    scripts-py) out="$(NO_COLOR=1 timeout 300 python -m unittest discover -s Tests/scripts -p "test_*.py" 2>&1)"; echo "$out" | grep -E "^(Ran|OK|FAILED)"; { echo "$out" | grep -q "^Ran [0-9]* test" && ! echo "$out" | grep -q "FAILED"; } || fail=1;;  # T-751
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
    # T-520 (52 §52.5): the wasm tools; most machines have no Emscripten SDK, so its absence is a skip.
    wasm)      wasm_emcmake=0
               if command -v emcmake >/dev/null 2>&1 || { [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; } ||
                  { [ -n "${EMSCRIPTEN_ROOT:-}" ] && [ -x "$EMSCRIPTEN_ROOT/emcmake" ]; }; then wasm_emcmake=1; fi
               if [ "$wasm_emcmake" = 0 ] || [ ! -f Tools/Scripts/build_wasm.sh ]; then
                 echo "wasm: SKIPPED (no emcmake: install the Emscripten SDK, or set EMSDK / EMSCRIPTEN_ROOT)"
               else
                 wasm_built=1
                 out="$(timeout 3600 bash Tools/Scripts/build_wasm.sh 2>&1 </dev/null)" ||
                   { fail=1; wasm_built=0; echo "$out" | tail -20; echo "wasm: build FAILED"; }
                 if [ "$wasm_built" = 1 ]; then
                   echo "$out" | grep "^build_wasm:"
                   out="$(timeout 1800 bash Tools/Scripts/check_wasm.sh 2>&1 </dev/null)" || fail=1
                   echo "$out" | grep -E "^check_wasm:|MISMATCH" | head -40
                 fi
               fi;;
    *) echo "unknown suite $s"; fail=1;;
  esac
}
# T-361: the suites below run at the same time; the rest (GL contexts, perf / slow timing cases, the
# agent endpoint and ctest, which runs executables of its own) afterwards, one after the other.
PARALLEL_SUITES=" core import bundle capi flow editor editor_service editor_qt history net symbols templates lottie golden text gpu headers layering testtemp import-py scripts-py ir roundtrip shape skills qt512 "
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
# 2026-11-20: what passed, on which sources (Tools/Scripts/check_stamp.py). merge_task.sh reads it
# back: the merge is refused when the sources changed after the last passing run or when a suite the
# changed files need never ran. Skipped suites were dropped from $suites above and are not recorded.
if [ -f Tools/Scripts/check_stamp.py ]; then
  if [ "$fail" = 0 ] && [ ${#suites[@]} -gt 0 ]; then
    python Tools/Scripts/check_stamp.py write --suites "${suites[*]}" --config "$CFG$([ "$NO_BUILD_ARG" = 1 ] && echo ' (--no-build: binaries not rebuilt by this run)')" ||
      echo "dev_check: could not write the check stamp"
  elif [ "$fail" != 0 ]; then
    python Tools/Scripts/check_stamp.py clear
  fi
fi
echo; [ "$fail" = 0 ] && echo "DEV CHECK: PASS" || echo "DEV CHECK: FAIL"
exit $fail


  step "Command <-> op table check (docs/design/37-work-model.md §37.8.4, T-367)"
  bash tools/check_command_ops.sh || fail=1
#!/usr/bin/env bash
# Local merge gate for XTFrame (replaces GitHub Actions on the private repo).
#   tools/local_ci.sh            full run
#   tools/local_ci.sh --quick    MSVC Debug build + ctest only (no editor either, see below)
#   tools/local_ci.sh --clean    like the full run but rebuilds MSVC from scratch (use after a rebase/merge;
#                                MSBuild incremental builds are known to keep stale objects)
#   tools/local_ci.sh --no-editor  explicitly skip the editor steps (MSVC Qt5/Qt6, WSL Qt6); unlike a
#                                missing XT_QT_DIR this does NOT fail the gate. For callers (e.g. a
#                                worktree with no Qt installed) that intentionally don't cover the editor.
#   tools/local_ci.sh --fuzz     the long fuzz run, on its own (not a gate stage): configures/builds
#                                build/wsl-fuzz (WSL clang++, XT_BUILD_FUZZ=ON, GL/SHELL/TEXT off) and
#                                runs every libFuzzer target (tests/fuzz/run_fuzz.sh all) for
#                                XT_FUZZ_SECONDS (default 60) seconds each, after replaying
#                                tests/fuzz/regressions. See docs/design/15-testing.md §15.5,
#                                docs/tasks/T-39.md, docs/tasks/T-267b.md.
# XT_CI_ANDROID=1 adds an Android build stage (tools/build_android.sh, T-240), off by default.
# XT_CI_STAGE=fuzz-smoke or XT_CI_STAGE=sw-shell runs only that stage of the full run and exits
# (T-267b; for checking one stage without the whole gate).
# Steps: MSVC Debug + Release build and ctest (vs2022 preset), MSVC editor build + ctest -R editor
# against Qt 5.12 (XT_QT_DIR) and, if installed, Qt 6 (XT_QT6_DIR), clang++ syntax check of every
# first-party .cpp with the Linux warning set and -DNDEBUG, and, when WSL has cmake+ninja, a GCC
# Debug build + ctest inside WSL (plus the editor against WSL's Qt 6, if found). See
# docs/design/17-editor.md §17.9. The full run (not --quick) then also has the fuzz smoke stage
# (T-267b: WSL clang++ build/wsl-fuzz, every target of FUZZ_SMOKE_TARGETS for
# XT_FUZZ_SMOKE_SECONDS, default 20, seconds; any crash / timeout / leak fails the gate; skipped
# when WSL has no clang++) and the MSVC SW shell stage (T-267b: the vs2022-sw-shell preset,
# XT_SHELL_GL=OFF, the Android presentation path; xtframe-view and ctest -R xtframe-view_sw).
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
CLANG="${XT_CLANG:-C:/Users/Bigspring/scoop/apps/llvm/current/bin/clang++.exe}"
fail=0
CLEAN=""
QUICK=""
NO_EDITOR=""
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN="--clean-first" ;;
    --quick) QUICK=1 ;;
    --no-editor) NO_EDITOR=1 ;;
  esac
done
step() { echo; echo "=== $1"; }
# Note: never `tee /dev/stderr` here; when stderr is a redirected log file, opening /dev/stderr
# truncates it (the log ends up NUL-filled).
# tests/fuzz is excluded from the clang syntax check: those sources are Linux-only (unistd.h).

# The fuzz build (build/wsl-fuzz, incremental). Non-zero on a configure/build error.
fuzz_build() {
  wsl -d Ubuntu-24.04 -- bash -lc "cd '$WROOT' && cmake -S . -B build/wsl-fuzz -G Ninja -DXT_BUILD_FUZZ=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DXT_BUILD_GL=OFF -DXT_BUILD_SHELL=OFF -DXT_BUILD_TEXT=OFF && cmake --build build/wsl-fuzz"
}

# T-267b: the targets the gate's fuzz smoke stage runs (tests/fuzz/run_fuzz.sh names). A target
# with an unfixed finding stays in --fuzz but is left out here, with a comment line naming the
# reason and its tests/fuzz/findings/<target>/ file, until a later task fixes it.
FUZZ_SMOKE_TARGETS="ir xt lottie xtcl xts svg xtfx yaml image bundle_json import"

# T-267b: fuzz smoke stage of the full run.
fuzz_smoke_stage() {
  step "fuzz smoke (WSL clang++, build/wsl-fuzz, ${XT_FUZZ_SMOKE_SECONDS:-20}s per target)"
  if ! command -v wsl >/dev/null 2>&1 || ! wsl -d Ubuntu-24.04 -- bash -lc "command -v clang++ >/dev/null" 2>/dev/null; then
    echo "SKIP fuzz smoke: clang++ not found in WSL"
    return 0
  fi
  WROOT="$(wsl -d Ubuntu-24.04 -- wslpath -a "$ROOT" 2>/dev/null | tr -d '\r')"
  fuzz_build || { echo "fuzz build FAILED"; fail=1; return 0; }
  for target in $FUZZ_SMOKE_TARGETS; do
    step "fuzz smoke: $target (${XT_FUZZ_SMOKE_SECONDS:-20}s)"
    wsl -d Ubuntu-24.04 -- bash -lc "cd '$WROOT' && tests/fuzz/run_fuzz.sh $target ${XT_FUZZ_SMOKE_SECONDS:-20} build/wsl-fuzz" || fail=1
  done
}

# T-267b: the software presentation path (XT_SHELL_GL=OFF, the only one Android has): configure the
# vs2022-sw-shell preset, build xtframe-view (all the xtframe-view_sw_* tests run) and run those
# tests; none selected is a failure too.
sw_shell_stage() {
  step "MSVC SW shell (vs2022-sw-shell, XT_SHELL_GL=OFF)"
  cmake --preset vs2022-sw-shell >/dev/null || { fail=1; return 0; }
  find build/vs2022-sw-shell -path "*/Debug/xtframe-view.exe" -delete 2>/dev/null
  cmake --build --preset vs2022-sw-shell-debug --target xtframe-view --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
  local out
  out="$(ctest --preset vs2022-sw-shell-debug -R xtframe-view_sw --no-tests=error --output-on-failure 2>&1 | grep -E "tests passed|Failed|\*\*\*|Not Run|No tests were found")"
  echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1
}

case "${XT_CI_STAGE:-}" in
  "") ;;
  fuzz-smoke) fuzz_smoke_stage ;;
  sw-shell) sw_shell_stage ;;
  *) echo "unknown XT_CI_STAGE '${XT_CI_STAGE}' (fuzz-smoke | sw-shell)"; exit 2 ;;
esac
if [ -n "${XT_CI_STAGE:-}" ]; then
  echo; if [ "$fail" -eq 0 ]; then echo "LOCAL CI ($XT_CI_STAGE): PASS"; else echo "LOCAL CI ($XT_CI_STAGE): FAIL"; fi
  exit $fail
fi

if [ "${1:-}" = "--fuzz" ]; then
  step "WSL clang fuzz build (build/wsl-fuzz)"
  command -v wsl >/dev/null 2>&1 || { echo "wsl not found"; exit 1; }
  WROOT="$(wsl -d Ubuntu-24.04 -- wslpath -a "$ROOT" 2>/dev/null | tr -d '\r')"
  fuzz_build || fail=1
  if [ "$fail" -eq 0 ]; then
    step "libFuzzer: all targets (${XT_FUZZ_SECONDS:-60}s each, regressions replayed first)"
    wsl -d Ubuntu-24.04 -- bash -lc "cd '$WROOT' && tests/fuzz/run_fuzz.sh all ${XT_FUZZ_SECONDS:-60} build/wsl-fuzz" || fail=1
  fi
  echo; if [ "$fail" -eq 0 ]; then echo "LOCAL CI --fuzz: PASS"; else echo "LOCAL CI --fuzz: FAIL"; fi
  exit $fail
fi

step "MSVC Debug"
# T-267a: this stage is also the "R builds without E" check (docs/RELEASE-MAP.md §6.1): the vs2022
# preset leaves XT_BUILD_EDITOR at its default OFF, and the build below compiles every target of that
# tree, xtframe-pack, xtframe-view and xtframe-compile included, so a runtime target that needs an
# editor target fails the gate here. (A cache configured with -DXT_BUILD_EDITOR=ON by hand would
# defeat it; the preset never sets it.)
cmake --preset vs2022 >/dev/null || fail=1
find build/vs2022 -path "*/Debug/*.exe" -delete 2>/dev/null
cmake --build --preset vs2022-debug --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
# MSBuild occasionally reports success without relinking an executable ("Not Run" in ctest);
# one incremental rebuild fixes that, so retry once before failing.
# $2 (optional) is a -R filter, e.g. "editor" for the editor-only presets below.
run_ctest() { local extra=""; [ -n "${2:-}" ] && extra="-R $2"; ctest --preset "$1" --output-on-failure $extra 2>&1 | grep -E "tests passed|Failed|\*\*\*|Not Run"; }
out="$(run_ctest vs2022-debug)"
if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-debug --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-debug)"; fi
echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1

if [ -z "$QUICK" ]; then
  step "MSVC Release"
  # MSBuild sometimes skips relinking test executables after a static library changed; force it.
  find build/vs2022 -path "*/Release/*.exe" -delete 2>/dev/null
  cmake --build --preset vs2022-release --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
  out="$(run_ctest vs2022-release)"
  if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-release --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-release)"; fi
  echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1

  step "MSVC editor (Qt5)"
  if [ -n "$NO_EDITOR" ]; then
    echo "SKIP editor: --no-editor"
  elif [ -z "${XT_QT_DIR:-}" ]; then
    echo "SKIP editor: XT_QT_DIR not set"
    fail=1
  else
    cmake --preset vs2022-editor >/dev/null || fail=1
    find build/vs2022-editor -path "*/Debug/*.exe" -delete 2>/dev/null
    cmake --build --preset vs2022-editor-debug --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
    out="$(run_ctest vs2022-editor-debug editor)"
    if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-editor-debug --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-editor-debug editor)"; fi
    echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1

    find build/vs2022-editor -path "*/Release/*.exe" -delete 2>/dev/null
    cmake --build --preset vs2022-editor-release --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
    out="$(run_ctest vs2022-editor-release editor)"
    if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-editor-release --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-editor-release editor)"; fi
    echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1
  fi

  step "MSVC editor (Qt6)"
  if [ -n "$NO_EDITOR" ]; then
    echo "SKIP editor (Qt6): --no-editor"
  elif [ -z "${XT_QT6_DIR:-}" ]; then
    echo "SKIP editor (Qt6): XT_QT6_DIR not set"
  else
    cmake --preset vs2022-editor-qt6 >/dev/null || fail=1
    find build/vs2022-editor-qt6 -path "*/Debug/*.exe" -delete 2>/dev/null
    cmake --build --preset vs2022-editor-qt6-debug --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
    out="$(run_ctest vs2022-editor-qt6-debug editor)"
    if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-editor-qt6-debug --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-editor-qt6-debug editor)"; fi
    echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1

    find build/vs2022-editor-qt6 -path "*/Release/*.exe" -delete 2>/dev/null
    cmake --build --preset vs2022-editor-qt6-release --parallel $CLEAN 2>&1 | grep -iE " error |warning C" | grep -v "_deps" && fail=1
    out="$(run_ctest vs2022-editor-qt6-release editor)"
    if echo "$out" | grep -q "Not Run"; then echo "(ctest: missing executable, rebuilding once)"; cmake --build --preset vs2022-editor-qt6-release --parallel >/dev/null 2>&1; out="$(run_ctest vs2022-editor-qt6-release editor)"; fi
    echo "$out"; echo "$out" | grep -q "100% tests passed" || fail=1
  fi

  step "public header check (docs/design/23-host-integration.md §23.6)"
  bash tools/check_public_headers.sh || fail=1

  step "layering check (docs/RELEASE-MAP.md §6.2, T-274)"
  bash tools/check_layering.sh || fail=1

  step "skill copy check (docs/design/24-design-import.md §24.8)"
  bash tools/check_skill_sync.sh || fail=1

  step "skill operation list check (docs/tasks/T-360.md item 9)"
  bash tools/check_ops_doc.sh || fail=1

  step "Command <-> op table check (docs/design/37-work-model.md §37.8.4, T-367)"
  bash tools/check_command_ops.sh || fail=1

  step "standard symbol index check (docs/tasks/T-237.md item 6)"
  bash tools/check_symbols_index.sh || fail=1

  step "editor translation check (docs/design/17-editor.md §17.8a)"
  if [ -n "$NO_EDITOR" ]; then
    echo "SKIP i18n: --no-editor"
  elif { [ -n "${XT_QT_DIR:-}" ] && { [ -x "$XT_QT_DIR/bin/lrelease.exe" ] || [ -x "$XT_QT_DIR/bin/lrelease" ]; }; } || command -v lrelease >/dev/null 2>&1; then
    bash tools/check_i18n.sh || fail=1
  else
    echo "SKIP i18n: Qt lrelease not found (XT_QT_DIR/bin or PATH)"
  fi

  step "clang++ syntax check (-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -DNDEBUG)"
  INC="-Icore/include -Icore/src -Iflow/include -Ibundle/include -Ibundle/src -Iimport/include -Iimport/src -Ieditor/core/include -Ieditor/core/src -Ieditor/service/include -Ieditor/service/src -Ieditor/qt/include -Ieditor/qt/src -Ieditor/app -Icompile/include -Icompile/src -Icompile/src/xts -Isw/include -Isw/src -Igl/include -Igl/src -Igpu/include -Ishell/include -Itext/include -Itext/src -Itests/golden -Itests/perf -Itests/common -Ieditor/mcp -Ieditor/history/include -Ieditor/history/src -Itests/history"
  SYS="-isystem third_party/nlohmann -isystem third_party/stb -isystem third_party/doctest -isystem third_party/bc7enc"
  for d in build/vs2022*/_deps/xt_cpp_httplib-src build/vs2022*/_deps/xt_miniz_src-src build/vs2022*/_deps/xt_clipper2_src-src/CPP/Clipper2Lib/include build/vs2022/_deps/*/include build/vs2022/_deps/*-src/include build/vs2022/_deps/*-build/include build/vs2022/_deps/harfbuzz-src/src third_party/sheenbidi/Headers third_party/libunibreak/src; do
    [ -d "$d" ] && SYS="$SYS -isystem $d"
  done
  SYS="$SYS -isystem build/vs2022/_deps/sdl2-build/include-config-debug -isystem build/vs2022/_deps/sdl2-build/include"
  # editor/qt, editor/app, editor/browse and tests/editor_qt include Qt headers (T-60a); they are
  # only checked when XT_QT_DIR points at a Qt installation, and skipped (with a note) otherwise.
  QT_SOURCES_PRUNE="-path editor/qt -o -path editor/app -o -path editor/browse -o -path tests/editor_qt"
  if [ -n "${XT_QT_DIR:-}" ] && [ -d "$XT_QT_DIR/include" ]; then
    SYS="$SYS -isystem $XT_QT_DIR/include -isystem $XT_QT_DIR/include/QtCore -isystem $XT_QT_DIR/include/QtGui -isystem $XT_QT_DIR/include/QtWidgets"
    DEFS_QT="-D_HAS_DEPRECATED_RESULT_OF=1 -DQT_CORE_LIB -DXT_EDITOR_QT_HISTORY -DXT_EDITOR_QT_GL -DXT_EDITOR_QT_TEXT -DQT_DISABLE_DEPRECATED_BEFORE=0x050C00 -DQT_NO_CAST_FROM_ASCII"
    QT_SOURCES_PRUNE=""
  else
    DEFS_QT=""
    echo "(XT_QT_DIR unset: skipping the Qt editor sources in the clang syntax check)"
  fi
  if [ -n "$QT_SOURCES_PRUNE" ]; then
    FIND_SOURCES=$(find core flow bundle import editor compile sw gl shell text tools tests \( $QT_SOURCES_PRUNE \) -prune -o -name '*.cpp' -not -path '*/third_party/*' -not -path '*/build/*' -not -path 'tests/fuzz/*' -print)
  else
    FIND_SOURCES=$(find core flow bundle import editor compile sw gl shell text tools tests -name '*.cpp' -not -path '*/third_party/*' -not -path '*/build/*' -not -path 'tests/fuzz/*')
  fi
  # One clang process per file, XT_CLANG_JOBS at a time (default: all cores).
  check_one() {
    local out; out=$("$CLANG" -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -Wno-c2y-extensions -Wno-missing-designated-field-initializers -DNDEBUG -DDOCTEST_CONFIG_USE_STD_HEADERS -D__PRFCHWINTRIN_H -DXT_GOLDEN_DIR=\"x\" -DXT_GOLDEN_LOTTIE_DIR=\"x\" -DXT_GOLDEN_EXPECTED_DIR=\"x\" -DXT_GOLDEN_UI_DIR=\"x\" -DXT_TEST_DATA_DIR=\"x\" -DXT_TEST_XT_DIR=\"x\" -DXT_TEST_FONTS_DIR=\"x\" -DXT_TEST_LAYOUT_DIR=\"x\" -DXT_PERF_FIXTURES_DIR=\"x\" -DXT_TEST_FLOW_DIR=\"x\" -DXT_TEST_BUNDLE_DIR=\"x\" -DXT_TEST_IMPORT_DIR=\"x\" -DXT_TEST_EDITOR_DIR=\"x\" -DXT_EDITOR_TEMPLATES_DIR=\"x\" -DXT_SCHEMAS_DIR=\"x\" -DXT_TEST_EDITOR_QT_DIR=\"x\" -DXT_EXAMPLES_DIR=\"x\" -DXT_GPU_GOLDEN_DIR=\"x\" -DXT_GPU_ACTUAL_DIR=\"x\" -DXT_GPU_UI_DIR=\"x\" -DXT_EDITOR_I18N_DIR=\"x\" -DXT_CONTRACTS_DIR=\"x\" -DXT_TEST_IMPORT_DC_DIR=\"x\" -DXT_TEST_GOLDEN_LOTTIE_DIR=\"x\" -DXT_TEST_LOTTIE_DIR=\"x\" -DXT_SYMBOLS_DIR=\"x\" -DXT_SYMBOLS_TEST_DIR=\"x\" -DXT_SYMBOLS_OUT_DIR=\"x\" -DXT_STARTER_DIR=\"x\" -DXT_DESKTOP_APP_UI_DIR=\"x\" -DXT_XTFRAME_IMPORT_EXE=\"x\" -DXT_NULL_DEVICE=\"x\" -DXT_EDITOR_ICONS_DIR=\"x\" -DXT_TEST_IMPORT_LOTTIE_DIR=\"x\" -DXT_MOBILE_HUD_DIR=\"x\" -DXT_STARTER_PROJECT_DIR=\"x\" -DUSINGZ $DEFS_QT $INC $SYS "$1" 2>&1 | grep -E "error:" | grep -v "doctest.h\|__COUNTER__\|c2y-extensions" | head -3)
    if [ -n "$out" ]; then echo "FAIL $1"; echo "$out"; fi
  }
  export -f check_one; export CLANG INC SYS DEFS_QT
  n=$(echo "$FIND_SOURCES" | grep -c .)
  res="$(echo "$FIND_SOURCES" | xargs -P "${XT_CLANG_JOBS:-$(nproc)}" -I{} bash -c 'check_one "$1"' _ {})"
  [ -n "$res" ] && echo "$res"
  bad=$(echo "$res" | grep -c '^FAIL ')
  echo "checked $n files, $bad with errors"
  [ "$bad" -ne 0 ] && fail=1

  if command -v wsl >/dev/null 2>&1 && wsl -d Ubuntu-24.04 -- bash -lc "command -v cmake >/dev/null && command -v ninja >/dev/null" 2>/dev/null; then
    step "WSL GCC Debug"
    WROOT="$(wsl -d Ubuntu-24.04 -- wslpath -a "$ROOT" 2>/dev/null | tr -d '\r')"
    # §17.9 second matrix leg: if WSL has a Qt6 CMake config, build the editor there too (offscreen
    # ctest, same as Windows: the ENVIRONMENT QT_QPA_PLATFORM=offscreen on xt_editor_qt_tests
    # applies on every platform).
    # OFF is passed explicitly so an earlier run's cached ON does not outlive a skip.
    WSL_EDITOR_DEF="-DXT_BUILD_EDITOR=OFF"
    if [ -n "$NO_EDITOR" ]; then
      echo "SKIP editor (WSL): --no-editor"
    else
      # The directory holding Qt6Config.cmake: $Qt6_DIR, qmake6's library dir, or the usual CMake
      # prefixes. The probe goes in on stdin so neither Windows nor WSL's login shell rewrites its
      # quotes and `$`s.
      WSL_QT6_DIR="$(wsl -d Ubuntu-24.04 -- bash -ls 2>/dev/null <<'EOF' | tr -d '\r'
for dir in "${Qt6_DIR:-}" "$(qmake6 -query QT_INSTALL_LIBS 2>/dev/null)/cmake/Qt6" /usr/lib/cmake/Qt6 \
           "/usr/lib/$(gcc -dumpmachine 2>/dev/null)/cmake/Qt6" /usr/local/lib/cmake/Qt6; do
  if [ -f "$dir/Qt6Config.cmake" ]; then echo "$dir"; exit 0; fi
done
EOF
)"
      if [ -n "$WSL_QT6_DIR" ]; then
        echo "(WSL Qt6: $WSL_QT6_DIR)"
        WSL_EDITOR_DEF="-DXT_BUILD_EDITOR=ON -DQt6_DIR=$WSL_QT6_DIR"
      else
        echo "SKIP editor (WSL): Qt6 not found (no Qt6Config.cmake via \$Qt6_DIR, qmake6, /usr/lib or /usr/local/lib)"
      fi
    fi
    wsl -d Ubuntu-24.04 -- bash -lc "cd '$WROOT' && cmake -S . -B build/wsl-gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ $WSL_EDITOR_DEF >/dev/null && cmake --build build/wsl-gcc 2>&1 | grep -E 'error:|FAILED:' | head -5; cd build/wsl-gcc && ctest --output-on-failure 2>&1 | grep -E 'tests passed|Failed|\*\*\*'" | tee >(cat >&2) | grep -q "100% tests passed" || fail=1
    # A compile or link error inside the WSL build must fail the run even if stale test binaries still pass.
    wsl -d Ubuntu-24.04 -- bash -lc "cd '$WROOT' && cmake --build build/wsl-gcc 2>&1 | grep -qE 'error:|FAILED:'" && { echo "WSL build reported errors"; fail=1; }
  else
    echo; echo "(WSL cmake/ninja not found: GCC step skipped. Install with: sudo apt install -y cmake ninja-build clang libx11-dev libxext-dev libgl1-mesa-dev xvfb)"
  fi

  fuzz_smoke_stage
  sw_shell_stage
fi

# T-240 (docs/tasks/T-240.md item 8): the Android test app, opt-in (needs the Android SDK/NDK/JDK
# and a host xtframe-pack; see platforms/android/README.md).
if [ "${XT_CI_ANDROID:-0}" = 1 ]; then
  step "Android build (XT_CI_ANDROID=1: tools/build_android.sh)"
  bash tools/build_android.sh || fail=1
fi

echo; if [ "$fail" -eq 0 ]; then echo "LOCAL CI: PASS"; else echo "LOCAL CI: FAIL"; fi
exit $fail

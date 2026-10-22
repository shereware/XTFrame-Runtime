#!/usr/bin/env bash
# Project-level golden corpus check (T-401, Docs/design/43-compatibility-and-versions.md 43.4).
#   Tools/Scripts/corpus_check.sh [--update] [--project <name>]... [--view EXE] [--pack EXE] [--work DIR]
# Packs every Tests/corpus/<project> source with `xtframe-pack --strict`, renders each UI headless
# at the listed frames, runs the behaviour scripts and compares screenshots (Tests/golden tolerance:
# XT_GOLDEN_CHANNEL / XT_GOLDEN_RATIO), the normalized manifest.json, ports.json and the script logs
# with expected/. --update or XT_CORPUS_UPDATE=1 rewrites expected/ instead; a PR that changes
# Tests/corpus/**/expected/ needs a "行為變更：" line in its body (Tools/Scripts/merge_task.sh).
# The executables default to the newest xtframe-view / xtframe-pack under bin/ (then build/).
# Exit codes: 0 pass (or updated), 1 a difference or a failed run, 2 usage / tools missing.
# The work lives in Tools/Scripts/corpus_check.py (Python 3, standard library only).
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."
PY=python3
command -v "$PY" >/dev/null 2>&1 && "$PY" -c "import sys" >/dev/null 2>&1 || PY=python
exec "$PY" Tools/Scripts/corpus_check.py "$@"

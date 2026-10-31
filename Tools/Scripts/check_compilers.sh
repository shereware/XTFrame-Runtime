#!/usr/bin/env bash
# Two native builds of the same tree must produce the same bytes (Docs/design/15-testing.md §15.10; the
# GCC/MSVC-vs-Clang class of bug T-524 fixed): compiles Tests/xt/*.xt with both xtframe-compile builds, packs
# the three benchmark scenes with both xtframe-pack builds (--no-thumbs; T-527d --fonts bundled), and renders a few frames of each with
# both xtframe-bench builds (the SSE2 table, XT_SW_NO_AVX2=1). .xtir META time/CRC and manifest `builtAt`
# are blanked by Tools/Scripts/xt_normalize.js; everything else, including the last frame's pixels, must agree.
#
# Usage: Tools/Scripts/check_compilers.sh --a <bin dir> --b <bin dir> [--keep]
#   e.g. --a bin/linux_x64/Release --b bin/linux_x64/Release-clang (a tree configured with
#   -DCMAKE_CXX_COMPILER=clang++ -DXT_BIN_VARIANT=clang). Exit 0 when everything agrees, 1 on a mismatch,
#   2 on a usage/setup error. Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."
A=""; B=""; KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --a) A="$2"; shift ;;
    --b) B="$2"; shift ;;
    --keep) KEEP=1 ;;
    *) echo "usage: $0 --a <bin dir> --b <bin dir> [--keep]" >&2; exit 2 ;;
  esac
  shift
done
for d in "$A" "$B"; do
  for t in xtframe-compile xtframe-pack xtframe-bench; do
    [ -n "$d" ] && [ -x "$d/$t" ] || { echo "check_compilers: need $t in both bin dirs (--a/--b); missing in '${d:-?}'" >&2; exit 2; }
  done
done
command -v node >/dev/null 2>&1 || { echo "check_compilers: node is required for xt_normalize.js" >&2; exit 2; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-compilers.XXXXXX")"
failed=0
mismatch() { echo "  MISMATCH $1"; failed=1; }

# 1. compile
mkdir -p "$WORK/a" "$WORK/b"
n=0
for f in Tests/xt/*.xt; do
  s="$(basename "$f" .xt)"
  "$A/xtframe-compile" "$f" -o "$WORK/a/$s.xtir" > "$WORK/a/$s.log" 2>&1; ra=$?
  "$B/xtframe-compile" "$f" -o "$WORK/b/$s.xtir" > "$WORK/b/$s.log" 2>&1; rb=$?
  [ "$ra" = "$rb" ] || mismatch "compile $s: exit $ra vs $rb"
  if [ -f "$WORK/a/$s.xtir" ] || [ -f "$WORK/b/$s.xtir" ]; then
    node Tools/Scripts/xt_normalize.js "$WORK/a/$s.xtir" "$WORK/b/$s.xtir"
    cmp -s "$WORK/a/$s.xtir" "$WORK/b/$s.xtir" || mismatch "compile $s: .xtir differs"
  fi
  n=$((n + 1))
done
echo "check_compilers: compile $n files"

# 2. pack + 3. pixels
while read -r name src ui; do
  [ -n "$name" ] || continue
  "$A/xtframe-pack" "$src" -o "$WORK/a/pack-$name" --no-thumbs --fonts bundled > "$WORK/a/pack-$name.log" 2>&1 || mismatch "pack $name: build a failed ($WORK/a/pack-$name.log)"
  "$B/xtframe-pack" "$src" -o "$WORK/b/pack-$name" --no-thumbs --fonts bundled > "$WORK/b/pack-$name.log" 2>&1 || mismatch "pack $name: build b failed ($WORK/b/pack-$name.log)"
  node Tools/Scripts/xt_normalize.js "$WORK/a/pack-$name" "$WORK/b/pack-$name"
  if ! diff -rq "$WORK/a/pack-$name" "$WORK/b/pack-$name" > "$WORK/pack-$name.diff" 2>&1; then
    mismatch "pack $name: $(grep -c '' "$WORK/pack-$name.diff") files differ ($WORK/pack-$name.diff)"
  fi
  fa="$(XT_SW_NO_AVX2=1 "$A/xtframe-bench" "$WORK/a/pack-$name" "$ui" --frames 3 --warmup 2 --json 2>/dev/null | sed -n 's/.*"pixels_fnv":"\([0-9a-f]*\)".*/\1/p')"
  fb="$(XT_SW_NO_AVX2=1 "$B/xtframe-bench" "$WORK/b/pack-$name" "$ui" --frames 3 --warmup 2 --json 2>/dev/null | sed -n 's/.*"pixels_fnv":"\([0-9a-f]*\)".*/\1/p')"
  if [ -z "$fa" ] || [ "$fa" != "$fb" ]; then mismatch "pixels $name: ${fa:-?} vs ${fb:-?}"; fi
done <<'SCENES'
challenge-menu Examples/demos/challenge-menu/ui-project challenge-menu
game-ui Examples/demos/game-ui hud
fairy-forest-menu Examples/demos/fairy-forest-menu/ui-project demo3-menu
SCENES
echo "check_compilers: pack + pixels 3 scenes"
if [ "$failed" = 0 ]; then echo "check_compilers: OK ($A vs $B, byte-identical)"; else echo "check_compilers: FAILED"; fi
if [ "$KEEP" = 1 ]; then echo "work: $WORK"; else rm -rf "$WORK"; fi
exit $failed

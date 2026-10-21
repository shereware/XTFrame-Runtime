#!/usr/bin/env bash
# IR layout check (T-405, docs/design/43-compatibility-and-versions.md §43.5): every `.xtir` POD size
# and enum limit in core/include/xt/ir_pod.h must match both the committed tests/ir/layout.txt and the
# table of docs/design/02-ir-format.md §2.12, the minor stamped in §2.2 must be kIrFormatMinor, and
# tests/ir/fixtures/v2.<minor>.xtir must exist -- so a layout or enum change cannot land without the
# minor bump, the 02 chapter update and a frozen fixture in the same commit.
# Plain grep/awk, no configure or build. The layout is read from the header's one-line
# `static_assert(sizeof(<Name>) == <N>);` checks and from its `inline constexpr ... k<...>Last = <N>;`
# limits plus kIrFormatMinor; one `<kind> <name> <value>` line each, sorted.
#   tools/check_ir_layout.sh            compare (exit 0 PASS / 1 FAIL)
#   tools/check_ir_layout.sh --update   rewrite tests/ir/layout.txt from the header, then compare
# Prints the differences, then `IR LAYOUT: PASS` or `IR LAYOUT: FAIL (<n>)`.
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/.."

HDR=core/include/xt/ir_pod.h
LAYOUT=tests/ir/layout.txt
DOC=docs/design/02-ir-format.md
fail=0

generated() {
  {
    grep -E '^static_assert\(sizeof\([A-Za-z0-9_]+\) == [0-9]+\);' "$HDR" |
      sed -E 's/^static_assert\(sizeof\(([A-Za-z0-9_]+)\) == ([0-9]+)\);.*/struct \1 \2/'
    grep -E '^inline constexpr u?int[0-9]+_t (kIrFormatMinor|k[A-Za-z0-9]*Last) = [0-9]+;' "$HDR" |
      sed -E 's/^inline constexpr u?int[0-9]+_t ([A-Za-z0-9_]+) = ([0-9]+);.*/limit \1 \2/'
  } | LC_ALL=C sort
}

# The 02 table between the ir-layout markers: `| struct | `IrFoo` | 24 |` rows.
documented() {
  tr -d '\r' <"$DOC" | awk '/<!-- ir-layout:begin -->/ { on = 1; next } /<!-- ir-layout:end -->/ { on = 0 } on' |
    grep -E '^\| *(struct|limit) *\|' |
    sed -E 's/^\| *(struct|limit) *\| *`?([A-Za-z0-9_]+)`? *\| *([0-9]+) *\|.*/\1 \2 \3/' |
    LC_ALL=C sort
}

gen="$(generated)"
if [ -z "$gen" ]; then
  echo "$HDR: no static_assert(sizeof(...)) found"
  echo "IR LAYOUT: FAIL (1)"
  exit 1
fi
if [ "${1:-}" = "--update" ]; then
  printf '%s\n' "$gen" >"$LAYOUT"
  echo "rewrote $LAYOUT ($(printf '%s\n' "$gen" | grep -c .) lines)"
fi

report() {  # report <label> <expected text> <actual text>
  local d
  d="$(diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | grep -E '^[<>]')"
  if [ -n "$d" ]; then
    echo "$1 (< $HDR, > the other side):"
    printf '%s\n' "$d" | sed 's/^/  /'
    fail=$((fail + $(printf '%s\n' "$d" | grep -c .)))
  fi
}

if [ ! -f "$LAYOUT" ]; then
  echo "$LAYOUT is missing (run tools/check_ir_layout.sh --update)"
  fail=$((fail + 1))
else
  report "$LAYOUT differs from the header" "$gen" "$(tr -d '\r' <"$LAYOUT" | grep -v '^$')"
fi
report "$DOC §2.12 table differs from the header" "$gen" "$(documented)"

minor="$(printf '%s\n' "$gen" | awk '$2 == "kIrFormatMinor" { print $3 }')"
if ! grep -qE "^\| 6 \| uint16 \| minor = \*\*${minor}\*\*" "$DOC"; then
  echo "$DOC §2.2: the header row does not say minor = **${minor}** (kIrFormatMinor)"
  fail=$((fail + 1))
fi
if [ ! -f "tests/ir/fixtures/v2.${minor}.xtir" ]; then
  echo "tests/ir/fixtures/v2.${minor}.xtir is missing (a frozen fixture per minor, 43 §43.5)"
  fail=$((fail + 1))
fi

if [ "$fail" = 0 ]; then
  echo "IR LAYOUT: PASS ($(printf '%s\n' "$gen" | grep -c .) entries, minor ${minor})"
  exit 0
fi
echo "IR LAYOUT: FAIL ($fail)"
exit 1

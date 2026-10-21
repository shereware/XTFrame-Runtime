#!/usr/bin/env bash
# Layering check (docs/RELEASE-MAP.md §6.1, §6.2 points 1 and 2; docs/tasks/T-274.md): the runtime
# layer R (future open source) never includes or links the editor layer E, Pro (P) or the built-in
# AI (A). Plain grep / awk over the source tree, no configure needed:
#   1. includes: no `#include "xt/editor/..."`, "xt/editor_qt/", "xt/editor_service/", "xt/import/",
#      "xt/pro/" or "xt/ai/" (quoted or angle-bracketed) in any C/C++ file under an R directory.
#   2. links: no `xt_editor_*`, `xt_import`, `xt_pro_*`, `xt_ai`, `Qt5::` or `Qt6::` in a
#      CMakeLists.txt under an R directory, nor in a target_link_libraries() of an R tool in
#      tools/CMakeLists.txt (that file also builds E tools, e.g. xtframe-import, so it is checked
#      per target: R_TOOLS below).
#   3. core/ without <filesystem> (docs/tasks/T-263e.md ruling B): xt_core has to build where the
#      standard library has no usable std::filesystem (console SDKs, some Android NDK setups), so
#      no C/C++ file under core/ includes <filesystem> (or <experimental/filesystem>). File status
#      reaches the core through FileAssetResolver's injected FileStatusFn.
#   4. directories (T-267a): every top-level directory holding C/C++ sources or a CMakeLists.txt is
#      in R_DIRS, E_DIRS or NEUTRAL_DIRS below, and every such tools/<name>/ or platforms/<name>/ is
#      in R_DIRS or E_DIRS, so a new directory cannot slip past checks 1 and 2 unclassified.
# §6.2 point 3 (P features entered through xt::license::has) waits until an xt_pro_* target exists.
# Prints every violation as `file:line: ...`, then `LAYERING: PASS` or `LAYERING: FAIL (<n>
# violation(s))`; exits 0 / 1.
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/.."

# The R directories of the RELEASE-MAP §6.1 table. gpu/ (xt_gpu, the GPU frame builder) is not in
# the table but is runtime layer, so it is listed too. tools/xtframe-bake is filed under E by that
# table (an E tool may use R, so there is nothing to check for it).
R_DIRS="core compile sw gpu gl bundle flow text shell platforms/android tools/common
        tools/xtframe-view tools/xtframe-compile tools/xtframe-pack tools/xtframe-gpu-probe
        tools/xtframe-design"
# The E directories of the same table (an E directory may use R; nothing is checked inside them).
# devtools/ (the throwaway AI endpoint of docs/design/20-agent-interface.md §20.1, which links the
# Qt editor) is not in the table; it is filed under E here (T-267a, see its report).
E_DIRS="editor import integrations devtools tools/xtframe-import tools/xtframe-bake"
# Directories that belong to no layer (documentation, tests, examples, formats, vendored code,
# build files and output, tooling scripts, repository metadata).
NEUTRAL_DIRS="docs tests examples schemas third_party cmake tools build .github .claude"
# Top-level directories whose subdirectories are classified one by one (tools/<name>/ is a tool).
SPLIT_DIRS="tools platforms"
# The R tools defined in tools/CMakeLists.txt (checked per target there).
R_TOOLS="xtframe-view xtframe-compile xtframe-pack xtframe-gpu-probe xtframe-design"
INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]xt/(editor|editor_qt|editor_service|import|pro|ai)/'
LINK_RE='xt_editor_|xt_import|xt_pro_|xt_ai([^A-Za-z0-9_]|$)|Qt5::|Qt6::'

viol="$(mktemp)"
trap 'rm -f "$viol"' EXIT

dirs=()
for d in $R_DIRS; do
  [ -d "$d" ] && dirs+=("$d")
done

# 1. Includes.
if [ ${#dirs[@]} -gt 0 ]; then
  grep -rnE "$INCLUDE_RE" \
    --include='*.h' --include='*.hpp' --include='*.hh' --include='*.hxx' --include='*.inl' --include='*.ipp' \
    --include='*.c' --include='*.cc' --include='*.cpp' --include='*.cxx' --include='*.m' --include='*.mm' \
    "${dirs[@]}" 2>/dev/null | sed 's/$/  [R includes E\/P\/A]/' >>"$viol"
fi

# 3. core/ never includes <filesystem> (T-263e).
if [ -d core ]; then
  grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](experimental/)?filesystem[>"]' \
    --include='*.h' --include='*.hpp' --include='*.hh' --include='*.hxx' --include='*.inl' --include='*.ipp' \
    --include='*.c' --include='*.cc' --include='*.cpp' --include='*.cxx' --include='*.m' --include='*.mm' \
    core 2>/dev/null | sed 's/$/  [core includes <filesystem>]/' >>"$viol"
fi

# 2a. Links in the CMakeLists.txt files under the R directories (comments stripped).
if [ ${#dirs[@]} -gt 0 ]; then
  while IFS= read -r f; do
    awk -v re="$LINK_RE" -v file="$f" '
      { line = $0; sub(/#.*/, "", line) }
      line ~ re { printf "%s:%d: %s  [R links E/P/A/Qt]\n", file, NR, $0 }
    ' "$f" >>"$viol"
  done < <(grep -rlE "$LINK_RE" --include='CMakeLists.txt' --include='*.cmake' "${dirs[@]}" 2>/dev/null)
fi

# 2b. tools/CMakeLists.txt: target_link_libraries() of an R tool, per target (a call may span lines).
if [ -f tools/CMakeLists.txt ]; then
  awk -v re="$LINK_RE" -v tools=" $R_TOOLS " -v file="tools/CMakeLists.txt" '
    {
      line = $0; sub(/#.*/, "", line)
      if (!inside && tolower(line) ~ /target_link_libraries[[:space:]]*\(/) {
        rest = line; sub(/^.*[Tt][Aa][Rr][Gg][Ee][Tt]_[Ll][Ii][Nn][Kk]_[Ll][Ii][Bb][Rr][Aa][Rr][Ii][Ee][Ss][[:space:]]*\(/, "", rest)
        target = rest; sub(/^[[:space:]]*/, "", target); sub(/[[:space:])].*$/, "", target)
        inside = 1; depth = 0; checked = rest
      } else if (inside) {
        checked = line
      } else {
        next
      }
      if (index(tools, " " target " ") > 0 && checked ~ re) {
        printf "%s:%d: %s  [R tool %s links E/P/A/Qt]\n", file, NR, $0, target
      }
      tmp = checked; opens = gsub(/\(/, "(", tmp); tmp = checked; closes = gsub(/\)/, ")", tmp)
      depth += opens - closes
      if (depth < 0) { inside = 0 }
    }
  ' tools/CMakeLists.txt >>"$viol"
fi

# 4. Directories (T-267a): tracked and untracked (not ignored) files, so build output never counts.
SRC_RE='(^|/)CMakeLists\.txt$|\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp|m|mm)$'
in_list() { # <item> <list>...
  local item="$1" d; shift
  for d in "$@"; do [ "$d" = "$item" ] && return 0; done
  return 1
}
sources="$(git ls-files --cached --others --exclude-standard 2>/dev/null | grep -E "$SRC_RE")"
if [ -z "$sources" ]; then
  echo "(not a git checkout: directory check skipped)"
else
  while IFS= read -r top; do
    if in_list "$top" $SPLIT_DIRS; then
      if echo "$sources" | grep -q "^$top/[^/]*$" && ! in_list "$top" $NEUTRAL_DIRS; then
        echo "$top/: holds C/C++ sources or a CMakeLists.txt directly and is in no list  [unclassified directory]" >>"$viol"
      fi
      while IFS= read -r sub; do
        in_list "$sub" $R_DIRS $E_DIRS ||
          echo "$sub/: holds C/C++ sources or a CMakeLists.txt and is in neither R_DIRS nor E_DIRS  [unclassified directory]" >>"$viol"
      done < <(echo "$sources" | awk -F/ -v t="$top" '$1 == t && NF > 2 { print $1 "/" $2 }' | sort -u)
    elif ! in_list "$top" $R_DIRS $E_DIRS $NEUTRAL_DIRS; then
      echo "$top/: holds C/C++ sources or a CMakeLists.txt and is in none of R_DIRS, E_DIRS, NEUTRAL_DIRS  [unclassified directory]" >>"$viol"
    fi
  done < <(echo "$sources" | awk -F/ 'NF > 1 { print $1 }' | sort -u)
fi

n=$(grep -c . "$viol")
if [ "$n" -eq 0 ]; then
  echo "LAYERING: PASS"
  exit 0
fi
cat "$viol"
echo "LAYERING: FAIL ($n violation(s))"
exit 1

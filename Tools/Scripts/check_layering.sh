#!/usr/bin/env bash
# Layering check (Docs/RELEASE-MAP.md §6.1, §6.2 points 1 and 2; Docs/tasks/T-274.md): the runtime
# layer R (future open source) never includes or links the editor layer E, Pro (P) or the built-in
# AI (A). Plain grep / awk over the source tree, no configure needed:
#   1. includes: no `#include "xt/editor/..."`, "xt/editor_qt/", "xt/editor_service/", "xt/import/",
#      "xt/pro/" or "xt/ai/" (quoted or angle-bracketed) in any C/C++ file under an R directory.
#   2. links: no `xt_editor_*`, `xt_import`, `xt_pro_*`, `xt_ai`, `Qt5::` or `Qt6::` in a
#      CMakeLists.txt under an R directory, nor in a target_link_libraries() of an R tool in
#      Code/Tools/CMakeLists.txt (that file also builds E tools, e.g. xtframe-import, so it is checked
#      per target: R_TOOLS below).
#   3. Code/Runtime/core/ without <filesystem> (Docs/tasks/T-263e.md ruling B): xt_core has to build where the
#      standard library has no usable std::filesystem (console SDKs, some Android NDK setups), so
#      no C/C++ file under Code/Runtime/core/ includes <filesystem> (or <experimental/filesystem>). File status
#      reaches the core through FileAssetResolver's injected FileStatusFn.
#   4. directories (T-267a): every top-level directory holding C/C++ sources or a CMakeLists.txt is
#      in R_DIRS, E_DIRS or NEUTRAL_DIRS below, and every such Code/Tools/<name>/,
#      Code/Runtime/<name>/ or Code/Runtime/platforms/<name>/ (SPLIT_DIRS) is in one of them too, so
#      a new directory cannot slip past checks 1 and 2 unclassified.
#   5. Code/Backends/ (T-439, 45 §45.7): a quoted include there names a public `xt/...` header or
#      another Code/Backends file (`xt_backend*.h`, `xt_platform_*.h`, `xt_renderer_*.h`), never a
#      module's `src/` or private header; host APIs (SDL2, Win32, webgpu.h) come in with <...>.
# §6.2 point 3 (P features entered through xt::license::has) waits until an xt_pro_* target exists.
# Prints every violation as `file:line: ...`, then `LAYERING: PASS` or `LAYERING: FAIL (<n>
# violation(s))`; exits 0 / 1.
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."

# The R directories of the RELEASE-MAP §6.1 table. Code/Runtime/gpu/ (xt_gpu, the GPU frame builder) is not in
# the table but is runtime layer, so it is listed too. Code/Tools/bake is filed under E by that
# table (an E tool may use R, so there is nothing to check for it).
R_DIRS="Code/Runtime/core Code/Runtime/compile Code/Runtime/sw Code/Runtime/gpu Code/Runtime/gl Code/Runtime/wgpu
        Code/Runtime/bundle Code/Runtime/flow Code/Runtime/text Code/Runtime/shell Code/Backends
        Code/Runtime/platforms/android Code/Tools/Common
        Code/Tools/view Code/Tools/compile Code/Tools/pack Code/Tools/gpu-probe
        Code/Tools/design"
# The E directories of the same table (an E directory may use R; nothing is checked inside them).
# Code/Editor/DevTools/ (the throwaway AI endpoint of Docs/design/20-agent-interface.md §20.1, which links the
# Qt editor) is not in the table; it is filed under E here (T-267a, see its report).
E_DIRS="Code/Editor Skills Code/Tools/import Code/Tools/bake"
# Directories that belong to no layer (documentation, tests, examples, formats, vendored code,
# build files and output, tooling scripts, repository metadata).
# Code/Tools is listed for the Code/Tools/CMakeLists.txt it holds directly (its subdirectories are
# classified one by one).
NEUTRAL_DIRS="Docs Tests Examples Assets Code/SDKs Code/Tools Tools build .github .claude"
# Directories whose subdirectories are classified one by one (Code/Tools/<name>/ is a tool,
# Code/Runtime/<name>/ a runtime module, Code/Runtime/platforms/<name>/ a platform).
SPLIT_DIRS="Code Code/Runtime Code/Runtime/platforms Code/Tools"
# The R tools defined in Code/Tools/CMakeLists.txt (checked per target there).
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

# 3. Code/Runtime/core/ never includes <filesystem> (T-263e).
if [ -d Code/Runtime/core ]; then
  grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](experimental/)?filesystem[>"]' \
    --include='*.h' --include='*.hpp' --include='*.hh' --include='*.hxx' --include='*.inl' --include='*.ipp' \
    --include='*.c' --include='*.cc' --include='*.cpp' --include='*.cxx' --include='*.m' --include='*.mm' \
    Code/Runtime/core 2>/dev/null | sed 's/$/  [core includes <filesystem>]/' >>"$viol"
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

# 2b. Code/Tools/CMakeLists.txt: target_link_libraries() of an R tool, per target (a call may span lines).
if [ -f Code/Tools/CMakeLists.txt ]; then
  awk -v re="$LINK_RE" -v tools=" $R_TOOLS " -v file="Code/Tools/CMakeLists.txt" '
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
  ' Code/Tools/CMakeLists.txt >>"$viol"
fi

# 5. Code/Backends/ includes only public headers, its own files and (angle-bracketed) host APIs.
if [ -d Code/Backends ]; then
  grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' \
    --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' --include='*.cpp' --include='*.mm' \
    Code/Backends 2>/dev/null |
    grep -vE '#[[:space:]]*include[[:space:]]*"(xt/[^"]+|xt_(backend|platform|renderer)[A-Za-z0-9_]*\.h)"' |
    sed 's/$/  [Backends include a private header]/' >>"$viol"
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
  # One line per source file: its classified directory (descending through SPLIT_DIRS), or
  # "DIRECT <dir>" for a file held directly by a split directory.
  while IFS= read -r line; do
    case "$line" in
      "DIRECT "*)
        top="${line#DIRECT }"
        in_list "$top" $NEUTRAL_DIRS ||
          echo "$top/: holds C/C++ sources or a CMakeLists.txt directly and is in no list  [unclassified directory]" >>"$viol";;
      *)
        in_list "$line" $R_DIRS $E_DIRS $NEUTRAL_DIRS ||
          echo "$line/: holds C/C++ sources or a CMakeLists.txt and is in none of R_DIRS, E_DIRS, NEUTRAL_DIRS  [unclassified directory]" >>"$viol";;
    esac
  done < <(echo "$sources" | awk -F/ -v splits=" $SPLIT_DIRS " '
    NF > 1 {
      p = $1; i = 1
      while (index(splits, " " p " ") > 0) {
        if (i + 1 == NF) { print "DIRECT " p; next }
        i++; p = p "/" $i
      }
      print p
    }' | sort -u)
fi

n=$(grep -c . "$viol")
if [ "$n" -eq 0 ]; then
  echo "LAYERING: PASS"
  exit 0
fi
cat "$viol"
echo "LAYERING: FAIL ($n violation(s))"
exit 1

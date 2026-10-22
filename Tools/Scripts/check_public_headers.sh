#!/usr/bin/env bash
# Public header check for the host contract in Docs/design/23-host-integration.md §23.6
# (Docs/tasks/T-100.md). Two greps, no compiler:
#   1. every header <xt/xt.h> aggregates may only include standard headers and other public
#      core headers -- never a `*_internal.h` and never a header that lives under a module's
#      `src/` (i.e. one that does not exist under Code/Runtime/core/include/).
#   2. Examples/engine_integration/main.cpp may only include the §23.6 surface: <xt/xt.h>, the
#      three non-core headers of the table, the few public tooling headers listed below, the shared
#      Examples/common/example_args.h, and standard headers.
# Prints every offending line with its line number and exits 1.
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."

# The core rows of the §23.6 table, plus the umbrella itself.
PUBLIC_HEADERS="xt.h context.h document.h element_ref.h input.h asset_resolver.h text_shaper.h command_list.h perf.h damage.h version.h"
# The non-core rows of the §23.6 table: a host includes the ones it uses.
EXAMPLE_ALLOWED="xt/xt.h xt/sw/sw_renderer.h xt/bundle/bundle.h xt/flow/flow_runtime.h xt/flow/host_ports.h"
# Public headers outside the frozen §23.6 runtime surface that engine_integration also uses: it
# compiles a .xt source (compile), writes its off-screen frame as a PNG (sw_png), demonstrates
# --simplify (command_simplifier) and, where XT_BUILD_TEXT is on, supplies a real shaper
# (ft_text_shaper). A host that ships pre-compiled .xtir and its own shaper needs none of them.
EXAMPLE_TOOLING="xt/command_simplifier.h xt/compile/compile.h xt/sw/sw_png.h xt/text/ft_text_shaper.h"
EXAMPLE="Examples/engine_integration/main.cpp"

viol="$(mktemp)"
trap 'rm -f "$viol"' EXIT

# includes <file> -> "line<TAB>quoted(0|1)<TAB>target" for every #include in the file.
includes() {
  grep -n '^[[:space:]]*#[[:space:]]*include' "$1" |
    sed -n 's/^\([0-9]*\):[[:space:]]*#[[:space:]]*include[[:space:]]*\(["<]\)\([^">]*\)[">].*/\1\t\2\t\3/p'
}

in_list() { # in_list <needle> <space separated list>
  case " $2 " in *" $1 "*) return 0;; *) return 1;; esac
}

# --- 1. the aggregated public headers -------------------------------------------------------
for h in $PUBLIC_HEADERS; do
  f="Code/Runtime/core/include/xt/$h"
  [ -f "$f" ] || { echo "$f: missing (listed in §23.6)" >>"$viol"; continue; }
  while IFS="$(printf '\t')" read -r ln open target; do
    [ -n "$ln" ] || continue
    case "$target" in
      xt/*)
        case "$target" in
          *_internal.h) echo "$f:$ln: includes the private header \"$target\"" >>"$viol"; continue;; esac
        [ -f "Code/Runtime/core/include/$target" ] ||
          echo "$f:$ln: includes \"$target\", which is not a public core header" >>"$viol"
        ;;
      *)
        [ "$open" = "<" ] ||
          echo "$f:$ln: includes \"$target\", which is not a public xt/ header" >>"$viol"
        ;;
    esac
  done <<EOT
$(includes "$f")
EOT
done

# --- 2. the fake host example ----------------------------------------------------------------
if [ -f "$EXAMPLE" ]; then
  while IFS="$(printf '\t')" read -r ln open target; do
    [ -n "$ln" ] || continue
    case "$target" in
      xt/*)
        in_list "$target" "$EXAMPLE_ALLOWED $EXAMPLE_TOOLING" ||
          echo "$EXAMPLE:$ln: includes \"$target\", which is not on the §23.6 host surface" >>"$viol"
        ;;
      *)
        [ "$open" = "<" ] || [ -f "$(dirname "$EXAMPLE")/$target" ] ||
          [ -f "Examples/common/$target" ] ||
          echo "$EXAMPLE:$ln: includes \"$target\", which is neither standard nor local" >>"$viol"
        ;;
    esac
  done <<EOT
$(includes "$EXAMPLE")
EOT
else
  echo "$EXAMPLE: missing" >>"$viol"
fi

if [ -s "$viol" ]; then
  cat "$viol"
  echo "PUBLIC HEADERS: FAIL"
  exit 1
fi
echo "PUBLIC HEADERS: PASS"

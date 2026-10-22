#!/usr/bin/env bash
# Test temp check (T-267d): no test uses the system temp directory directly. Every temp path a test
# needs goes under the per-process root of Tests/common/test_temp.h (xt::test::tempDir() /
# tempRoot()), which a passing run removes and which two worktrees running the same suite at the
# same time never share. Plain grep, no configure needed: any C/C++ file under Tests/ (except
# Tests/fuzz/, whose harnesses build without Tests/common, and Tests/common/ itself) that mentions
# `temp_directory_path`, `QDir::tempPath` or `QStandardPaths::TempLocation` is a violation.
# Prints every violation as `file:line: ...`, then `TEST TEMP: PASS` or `TEST TEMP: FAIL (<n>)`;
# exits 0 / 1.
# Not a C++ file: never run clang-format on it.
set -u
cd "$(dirname "$0")/../.."

viol="$(grep -rnE 'temp_directory_path|QDir::tempPath|QStandardPaths::TempLocation' Tests \
          --include='*.c' --include='*.cc' --include='*.cpp' --include='*.cxx' \
          --include='*.h' --include='*.hh' --include='*.hpp' --include='*.inl' 2>/dev/null |
        grep -vE '^Tests/(fuzz|common)/' |
        sed 's/$/  [use xt::test::tempDir() or tempRoot() from Tests\/common\/test_temp.h]/')"

if [ -z "$viol" ]; then
  echo "TEST TEMP: PASS"
  exit 0
fi
echo "$viol"
echo "TEST TEMP: FAIL ($(echo "$viol" | grep -c .))"
exit 1

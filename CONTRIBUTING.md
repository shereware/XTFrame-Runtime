# Contributing to XTFrame Runtime

Thank you for looking. A few things about how this repository works:

## This repository is a snapshot

Every commit here is exported from the XTFrame main repository by `tools/export_runtime.sh`
(the `Source-Commit:` trailer names the source commit). Changes are therefore not merged here
directly:

- **Issues are welcome**: bug reports, crashes, wrong rendering, portability problems, unclear
  documentation. A minimal `.xt` / `.xts` / Lottie file or Bundle that reproduces the problem helps
  most.
- **Pull requests are welcome as proposals.** An accepted change is applied in the main repository
  and arrives here with the next export, credited to you.

## Design documents are the specification

Behavior is specified in `docs/design/` (Traditional Chinese) before it is implemented; the code
follows the document. If what you want is not covered there, open an issue to discuss it first.
Naming, error handling (`Status` / `Result`; no exceptions or RTTI in `xt_core`) and memory rules
are in `docs/design/14-coding-standards.md`.

## Before you submit

- **Formatting:** `clang-format --dry-run --Werror` must be clean on every file you touched
  (style in `.clang-format`).
- **Tests:** configure, build and run `ctest` with one of the presets in `CMakePresets.json`; all
  tests must pass. CI runs the same on Linux (GCC and Clang), Windows and macOS.
- **Public headers:** a new public class or function under `*/include/xt/` needs a `///`
  documentation comment.
- **Language:** code, comments, identifiers, commit messages and user-facing text are English.
- **Dependencies:** `xt_core` takes no third-party code. Elsewhere, third-party code is vendored
  under `third_party/` or fetched with `FetchContent` at a pinned version; nothing system-installed.

## License

By contributing you agree that your contribution is licensed under the MIT license (see
`LICENSE`).

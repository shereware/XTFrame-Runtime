# Qt for the editor

Qt is never committed: everything in this directory except this file is git-ignored
(Docs/design/45-repository-layout.md §45.3). Put Qt installations here, or anywhere else and point a
variable at them.

## Versions (Docs/design/17-editor.md §17.9)

| Role | Version | Variable | Used by |
|---|---|---|---|
| Build target (full build, tests, deployment) | **Qt 5.15** LTS (5.15.2 or newer; 5.15.19 KDE patch collection on the dev machine) | `XT_QT_DIR` | `vs2022-editor` / `vs2022-agent` presets, `dev_check.sh`, `local_ci.sh`, `check_i18n.sh` (lupdate / lrelease) |
| Syntax floor (host editors ship 5.12) | Qt 5.12 (5.12.3 or newer) | `XT_QT512_DIR` | `local_ci.sh` stage "Qt 5.12 headers syntax check" (clang `-fsyntax-only`, headers only) |
| Second matrix leg | Qt 6 | `XT_QT6_DIR` | `vs2022-editor-qt6` preset, `local_ci.sh` "MSVC editor (Qt6)" |

The code policy does not follow the build target: only APIs present in **both Qt 5.12 and Qt 6**
(no API added in 5.13 or later; the list of forbidden APIs is in 17 §17.9). The build defines
`QT_DISABLE_DEPRECATED_BEFORE=0x060000`, so anything deprecated before Qt 6.0 does not compile.

A missing `XT_QT512_DIR` or `XT_QT6_DIR` makes its `local_ci.sh` stage print a `SKIP` line; only a
missing build-target Qt fails the gate.

## Layout and auto-detection

Each installation is a directory whose name starts with its version, holding the Qt prefix (the
directory with `lib/cmake/Qt5` and `bin/qmake[.exe]`) either directly or in a `Qt/` subdirectory:

```
Code/SDKs/Qt/5.15.19_pyside2/Qt/{bin,include,lib,mkspecs,plugins}
Code/SDKs/Qt/5.12.3_pyside2/Qt/{bin,include,lib,mkspecs,plugins}
```

When `XT_QT_DIR` is not set, `Tools/CMake/xt_qt.cmake` (and the scripts, with the same rule) takes the
newest `Code/SDKs/Qt/5.15*`, else the newest `Code/SDKs/Qt/5.12*`, else leaves the search to CMake
(`CMAKE_PREFIX_PATH`, `Qt5_DIR`, `Qt6_DIR` from the environment). Configure prints the choice:

```
-- xt_qt: using E:/.../Code/SDKs/Qt/5.15.19_pyside2/Qt (Qt 5.15.19)
```

`XT_QT512_DIR` falls back to the newest `Code/SDKs/Qt/5.12*` the same way. `XT_QT6_DIR` has no
fallback. A Qt prefix without this platform's `qmake` (a Windows Qt seen from WSL) is ignored.

A git worktree has its own (empty) `Code/SDKs/Qt/`: copy the installations in, or set `XT_QT_DIR` /
`XT_QT512_DIR` to the main checkout's.

## macOS on Apple Silicon

The official Qt 5.15.2 open-source binaries for macOS are x86_64 only. For a native arm64 build use a
self-built 5.15.x (the KDE Qt 5 patch collection builds for arm64) or Homebrew's `qt@5`
(`XT_QT_DIR=$(brew --prefix qt@5)`).

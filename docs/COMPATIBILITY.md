# XTFrame Compatibility Statement (beta)

Version 1 (2026-10-21). Applies from the first beta build onward. Traditional Chinese version:
[`COMPATIBILITY.zh-TW.md`](COMPATIBILITY.zh-TW.md). The normative source is
`docs/design/43-compatibility-and-versions.md` (§43.1, §43.3, §43.7); when the two differ, chapter 43
wins and this page is corrected.

This page says what stays working when you update XTFrame during the beta, what may still change,
and how to report a difference.

## 1. What counts as your work

Your work is the **source**: `.xt`, `.xts` and `.xtflow` files; the `<name>.xtui` project file and its
parts (`shared/`, `design/`, `.meta`, `manifest/assets.yaml`, `contracts/`, `conventions/`,
`AGENTS.md`); `.xteditor.json`; project symbols; imported Claude Design boards.

Bundles and `.xtir` files are **derived**: the editor can always rebuild them from the source.

## 2. The three promises

From the first beta onward:

1. **Every source file written by a beta or later version opens in every later version**, is migrated
   to the newest format automatically and can be written back.
2. **It behaves the same after opening as when it was written.** An existing behavior changes only to
   fix a bug, and only after the change has passed the project corpus comparison (a diff that is
   reviewed and approved) and has been recorded in the migration notes. Deliberate behavior changes
   are gated by the project's behavior version (section 6).
3. **Bundles keep working in the host.** The IR `major` version does not change during the beta; a
   `minor` version only appends at the end; a runtime reads every `minor` published since that
   runtime was released.

## 3. What is not promised

- Features marked **Experimental** (section 5), in the editor and in this page.
- Pro and AI features.
- The editor's window layout and its stored settings (QSettings).

## 4. Frozen during the beta

| Item | What is frozen | How it may change |
|---|---|---|
| Source formats | The fields and meaning of every source file listed in section 1 | New fields plus a migration; nothing removed, no meaning changed |
| Language | `.xt` elements and attributes, `.xts` properties and value sets, trigger names and actions, binding sources and converters, reserved names, port types | Additions only; behavior changes go through the behavior version (section 6) |
| Flow | Node types and their parameters, document connection point names | Additions only; renames keep an alias |
| IR | Major version 2; the layout of every chunk | A `minor` appends at the end |
| Bundle manifest | The fields of manifest version 2 | New fields |
| Runtime C++ API | The list in `docs/design/13-*.md` §13.6 | Appended at the end, with default implementations |
| Editor Service | Operation names and parameters | New operations; a retired name keeps working as an alias |
| Skills | The published instructions of the `xtui` and `xtui-editor` skills | Follow the operations |

## 5. Experimental

These features are outside the promises above. Files that use them still open, but their format and
behavior may change between beta versions. The editor shows an **Experimental** badge where you work
with them; clicking the badge opens this page.

| id | Feature | Marked in the editor |
|---|---|---|
| `rigging` | Rigging and deformation (bones, skinning, meshes; design chapter 32) | Properties: the Rig group |
| `modifier-runtime` | The runtime part of vector modifiers (every modifier not baked at edit time; design chapter 31) | Properties: the Modifiers group, when the stack holds a runtime modifier |
| `overlay-nesting` | The stacking order of nested `layer: overlay` layers | Not marked (a single property value) |
| `depth-layering-3d` | 3D UI depth layering | The 3D panel |
| `media-axis` | The media axis (video and image-sequence clips and their timeline sync) | Properties: the Clip group of a `<video>` or `<sequence>` |
| `window-snapshot` | The JSON content of the multi-window document snapshot, `Document::snapshotState()` (its version 1 may change) | Not marked (host API, no editor setting) |

## 6. Behavior version (`spec.behavior`)

A UI Project records the behavior it was made with in `<name>.xtui` as `spec.behavior`, an integer.
The first beta writes 1; a missing value means 0, the behavior before the beta. New projects get the
newest value; an existing project keeps its value until you upgrade it (UI Project panel, Project
tab: "Upgrade to latest", which explains the change first). The editor's About box shows the newest
value this build knows and the value of the open project.

The compiler writes the value into each UI's IR (the `META` chunk, an optional tail). A value of 0 is
not written, so older runtimes can still open the file; **a file with a value of 1 or more needs a
runtime that reads IR 2.8 or later**, so the promise for behavior versions counts from IR 2.8. Every UI
of a bundle carries the project's value.

Each value includes every flag before it. The table lists only changes to existing behavior, not new
features:

| Value | Flag | Behavior | Since |
|---|---|---|---|
| 1 | `flow-signal-coalescing` | Value-type document connection points push once per frame, merged; continuous-event triggers (pointer move, scroll, drag over) run once per frame with the last event | T-389 |
| 0 | — | Every event is pushed synchronously, one by one (before the beta) | — |

The behavior version goes up only when a behavior covered by the promises changes, and every step
comes with a corpus diff, a migration note and a row in this table.

## 7. Reporting a behavior difference

If a file looks or behaves differently after an update, please report it with:

1. **The versions**: in the editor, Help > About XTFrame > "Copy version info" (product, Qt, IR
   format and behavior versions); for the runtime viewer, the output of `xtframe-view --version`.
2. **The project's behavior version**: `spec.behavior` in `<name>.xtui` (or the "Behavior version" line
   in the UI Project panel, Project tab). Say whether you upgraded it.
3. **The version that last behaved as expected**, if you know it.
4. **A minimal project or file** that shows the difference, and what you expected to see.

Differences in Experimental features (section 5) are welcome as feedback, but they are not
compatibility bugs.

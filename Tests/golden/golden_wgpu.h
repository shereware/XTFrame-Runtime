#pragma once

#include <string>
#include <vector>

#include "golden_common.h"

/// T-438 (Docs/design/10-backends.md §10.10.3): the `--backend wgpu` path of xt_golden. One
/// headless WebGPU device for the whole run (XT_WGPU_BACKEND / XT_WGPU_FALLBACK as in
/// xt_wgpu_tests); every fixture's CommandList is drawn by `xt::wgpu::WgpuRenderer` into an RGBA8
/// target cleared to the fixture's colour and read back top-down.

namespace xt::golden {

/// Creates the device and fills `out`. False (and `err`, e.g. "no WebGPU adapter ...") when no
/// adapter or device is available, or when xt_golden was built without xt_wgpu.
bool makeWgpuBackend(FrameBackend& out, std::string& err);

/// The adapter `makeWgpuBackend` picked ("" before it ran).
std::string wgpuAdapterName();

/// Releases the device of `makeWgpuBackend` (call before exit).
void releaseWgpuBackend();

/// T-677 (Docs/tasks/T-677.md item 4): order independence over `fixtures` (UI fixtures of `uiDir`,
/// at least two). Each fixture is drawn alone by a fresh renderer; then, for every ordered pair
/// (A, B) with A != B, a fresh renderer draws A and then B, and B must equal its alone image byte
/// for byte. Prints one `[PASS] order A -> B` / `[FAIL] order A -> B: <n> bytes differ` line per
/// pair; returns the number of failed pairs (a fixture that cannot be rendered fails its pairs),
/// or -1 (with `err`) when a renderer cannot be made. Replaces the device of makeWgpuBackend.
int runWgpuOrderPairs(const std::vector<UiFixture>& fixtures, const std::string& uiDir, std::string& err);

}  // namespace xt::golden

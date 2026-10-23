#pragma once

#include <string>

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

}  // namespace xt::golden

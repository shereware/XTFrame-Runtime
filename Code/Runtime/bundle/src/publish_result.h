#pragma once

#include <filesystem>

#include "manifest_writer.h"
#include "xt/bundle/pack.h"

/// T-714 (Docs/design/22-bundle.md §22.17.2): the publish result of a pack() run -- the bundle's
/// size on disk, its bitmap images by kind and their estimated display memory (PackReport's
/// `bundleBytes`, `images*` and `gpuImageBytes*`). Internal to xt_bundle; pack() calls it once the
/// manifest is written and the run's removals are done.

namespace xt::bundle::detail {

/// Fills `report`'s publish result fields from `manifest` (the one written) and the files under
/// `outDir`. Reads only the headers of PNG, JPEG and DDS files; another bitmap format is decoded.
void measurePublishResult(const std::filesystem::path& outDir, const ManifestData& manifest,
                          PackReport& report);

}  // namespace xt::bundle::detail

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Atlas rectangle packing for pack() (T-199, docs/design/26-layers-symbols-library.md §26.3.4,
/// 22 §22.10 `atlas[]`): MaxRects (best short side fit, no rotation) over power-of-two pages up to
/// `maxSize`, `padding` pixels of empty space around every entry. Internal to xt_bundle.

namespace xt::bundle {

/// One image to pack.
struct AtlasInput {
    /// Caller's identity (the packed asset name).
    std::string id;
    int width = 0;
    int height = 0;
};

/// Where one input ended up.
struct AtlasPlacement {
    std::string id;
    /// The entry's own pixels (padding excluded), in the page.
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// One output page.
struct AtlasPage {
    int width = 0;
    int height = 0;
    std::vector<AtlasPlacement> entries;
};

/// Packs `inputs` into as few pages as possible. Inputs are placed largest-first (by max side,
/// then area); a page's size is the smallest power-of-two pair (width >= height) that fits its
/// entries, never above `maxSize` x `maxSize`. An input larger than `maxSize` minus padding is
/// returned in `tooLarge` and packed nowhere. `padding` >= 0 (clamped).
void packAtlas(const std::vector<AtlasInput>& inputs, int maxSize, int padding, std::vector<AtlasPage>& pages,
               std::vector<std::string>& tooLarge);

}  // namespace xt::bundle

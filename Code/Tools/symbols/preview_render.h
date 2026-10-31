#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xt/bundle/symbol_lint.h"

/// T-499 (Docs/design/49-symbol-sets.md 49.6): the headless render of `xtframe-symbols thumbnails`.
/// `xtframe-view --headless` keeps its frame loop inside main(), so there is no function to link;
/// this is the same pipeline written once for the symbol groups -- compileDocument -> Context ->
/// event script -> render -> xt_sw -- exactly the one `Tests/golden`'s `renderUiFixture` runs, so a
/// thumbnail is byte for byte the normal-state golden of its symbol.

namespace xt::tools::symbols {

struct Image {
    int width = 0;
    int height = 0;
    /// Straight-alpha RGBA8, row 0 = top, `width * height * 4` bytes.
    std::vector<uint8_t> rgba;
};

/// Renders `spec` (a `previews/<Name>.preview.json` of the group `groupDir`): compiles
/// `previews/<Name>.preview.xt` (the group directory is the asset root of fonts and textures), runs
/// the spec's ports and events (`port <name> <value>`, `placeholders`, `update <seconds>`), then
/// `spec.frames` updates of 1/60 s, and draws it into a `spec.width` x `spec.height` image cleared
/// to opaque white. False with `error` when it does not compile, load or run.
bool renderPreview(const std::filesystem::path& groupDir, const bundle::PreviewSpec& spec, Image& out,
                   std::string& error);

/// The group thumbnail: `cells` (at most four, in order) fitted into the quadrants of a
/// `width` x `height` image, aspect kept and area-averaged, the margin of a quadrant in the colour
/// of its image's top-left pixel. False when `cells` is empty or a size is not positive.
bool composeGroupThumbnail(const std::vector<Image>& cells, int width, int height, Image& out);

/// Writes `image` as a PNG. False when the file cannot be written.
bool writePng(const std::filesystem::path& path, const Image& image);

/// Reads a PNG. False when it cannot be read.
bool readPng(const std::filesystem::path& path, Image& out);

}  // namespace xt::tools::symbols

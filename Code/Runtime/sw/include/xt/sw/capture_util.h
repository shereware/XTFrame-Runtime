#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/types.h"

/// T-879 (Docs/design/59-visual-feedback.md §59.3, §59.11 V2): pixel tools for `view.capture` --
/// shrinking to a longest edge, cropping, the contact sheet and its built-in label font. Pure
/// functions on a tightly packed RGBA8 image, no dependency, the same output on every run.

namespace xt::sw {

/// A tightly packed RGBA8 image (stride == width * 4), row-major, y pointing down, non-premultiplied --
/// what SwRenderer writes into a target without `premultiplied` and what encodePng() takes.
struct CaptureImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

/// `src` shrunk so its longest edge is `maxEdge` px (the other edge rounded, at least 1), averaging
/// every source pixel by the area it covers (a box filter, in premultiplied space so transparent
/// pixels do not darken the edges). Never enlarges: a copy when the longest edge is within `maxEdge`
/// or `maxEdge` <= 0.
CaptureImage downscaleToMaxEdge(const CaptureImage& src, int maxEdge);

/// The pixels of `src` inside `rect` (pixel coordinates; the edges rounded outwards, the part outside
/// `src` dropped) into `out`. kInvalidArgument when nothing of `rect` lies inside `src` (`out` is left
/// unchanged).
Status crop(const CaptureImage& src, const Rect& rect, CaptureImage& out);

/// The gap between two cells of composeSheet(), px.
inline constexpr int kSheetGap = 4;

/// Lays `frames` (all the same size) out in a grid of `columns` columns (<= 0: ceil(sqrt(n))) on an
/// opaque mid-grey (#808080) with kSheetGap px between the cells, each frame drawn over the grey
/// (source-over) and `labels[i]` (when there is one) printed at its cell's top-left corner with
/// drawLabel() -- scale 2 when a cell is at least 256 px wide, else 1. kInvalidArgument for no frame,
/// an empty frame or frames of different sizes (`out` is left unchanged).
Status composeSheet(std::span<const CaptureImage> frames, std::span<const std::string> labels, int columns,
                    CaptureImage& out);

/// The width drawLabel() gives `text` at `scale`, without the 1 px border: 6 * scale px per character
/// (a 5 x 7 cell and one column of space), minus the last space.
int labelWidth(std::string_view text, int scale);

/// Prints `text` into `dst` with the built-in 5 x 7 dot font (`0-9 . : s - +`, space and `a-z`; any
/// other character is a blank cell), each dot `scale` x `scale` px (`scale` < 1 counts as 1), white
/// with a 1 px black border, opaque. (`x`, `y`) is the top-left of the first character's cell; the
/// border reaches one pixel further out. Clipped to `dst`. No font file or text engine: the same
/// pixels everywhere.
void drawLabel(CaptureImage& dst, int x, int y, std::string_view text, int scale = 1);

/// Every pixel of `img` drawn over the opaque colour (`r`, `g`, `b`) (source-over): the image becomes
/// opaque (for an encoding without alpha, e.g. JPEG).
void flattenOver(CaptureImage& img, uint8_t r, uint8_t g, uint8_t b);

}  // namespace xt::sw

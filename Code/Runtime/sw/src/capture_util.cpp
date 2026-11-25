#include "xt/sw/capture_util.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

/// T-879 (Docs/design/59-visual-feedback.md §59.3): see capture_util.h.

namespace xt::sw {

namespace {

size_t pixelIndex(int x, int y, int width) {
    return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
}

/// One axis of an area-averaging resample: for destination sample `d` of `dst` covering source
/// samples [d * src / dst, (d + 1) * src / dst), the first source sample and the weight of each.
struct AxisSpan {
    int first = 0;
    std::vector<double> weights;
};

std::vector<AxisSpan> axisSpans(int src, int dst) {
    std::vector<AxisSpan> spans(static_cast<size_t>(dst));
    const double step = static_cast<double>(src) / static_cast<double>(dst);
    for (int d = 0; d < dst; ++d) {
        const double a = static_cast<double>(d) * step;
        const double b = static_cast<double>(d + 1) * step;
        const int first = static_cast<int>(std::floor(a));
        const int last = std::min(src - 1, static_cast<int>(std::ceil(b)) - 1);
        AxisSpan& span = spans[static_cast<size_t>(d)];
        span.first = first;
        for (int s = first; s <= last; ++s) {
            const double lo = std::max(a, static_cast<double>(s));
            const double hi = std::min(b, static_cast<double>(s + 1));
            span.weights.push_back(std::max(0.0, hi - lo) / step);
        }
    }
    return spans;
}

uint8_t toByte(double v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

/// The 5 x 7 dots of `c`: seven rows, bit 4 the leftmost column; nullptr for a character the font
/// has no glyph for (a blank cell).
const uint8_t* glyphRows(char c) {
    static constexpr uint8_t kDigits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    };
    static constexpr uint8_t kLower[26][7] = {
        {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E},
        {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}, {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F},
        {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},
        {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},
        {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
        {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}, {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11},
        {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
        {0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01}, {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
        {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}, {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},
        {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
        {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
        {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}, {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    };
    static constexpr uint8_t kDot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    static constexpr uint8_t kColon[7] = {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    static constexpr uint8_t kMinus[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static constexpr uint8_t kPlus[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    if (c >= '0' && c <= '9') {
        return kDigits[c - '0'];
    }
    if (c >= 'a' && c <= 'z') {
        return kLower[c - 'a'];
    }
    switch (c) {
        case '.':
            return kDot;
        case ':':
            return kColon;
        case '-':
            return kMinus;
        case '+':
            return kPlus;
        default:
            return nullptr;
    }
}

}  // namespace

CaptureImage downscaleToMaxEdge(const CaptureImage& src, int maxEdge) {
    const int longest = std::max(src.width, src.height);
    if (maxEdge <= 0 || longest <= maxEdge || src.width <= 0 || src.height <= 0) {
        return src;
    }
    const double scale = static_cast<double>(maxEdge) / static_cast<double>(longest);
    const int dw = src.width >= src.height
                       ? maxEdge
                       : std::max(1, static_cast<int>(std::lround(static_cast<double>(src.width) * scale)));
    const int dh = src.height > src.width
                       ? maxEdge
                       : std::max(1, static_cast<int>(std::lround(static_cast<double>(src.height) * scale)));
    const std::vector<AxisSpan> xs = axisSpans(src.width, dw);
    const std::vector<AxisSpan> ys = axisSpans(src.height, dh);
    // Horizontal pass into premultiplied doubles (src.height rows of dw), then the vertical one.
    std::vector<double> mid(static_cast<size_t>(dw) * static_cast<size_t>(src.height) * 4, 0.0);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < dw; ++x) {
            const AxisSpan& span = xs[static_cast<size_t>(x)];
            double acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < span.weights.size(); ++k) {
                const size_t i = pixelIndex(span.first + static_cast<int>(k), y, src.width);
                const double a = static_cast<double>(src.rgba[i + 3]) / 255.0;
                const double w = span.weights[k];
                acc[0] += w * a * static_cast<double>(src.rgba[i]) / 255.0;
                acc[1] += w * a * static_cast<double>(src.rgba[i + 1]) / 255.0;
                acc[2] += w * a * static_cast<double>(src.rgba[i + 2]) / 255.0;
                acc[3] += w * a;
            }
            const size_t o = pixelIndex(x, y, dw);
            for (int c = 0; c < 4; ++c) {
                mid[o + static_cast<size_t>(c)] = acc[c];
            }
        }
    }
    CaptureImage out;
    out.width = dw;
    out.height = dh;
    out.rgba.assign(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4, 0);
    for (int y = 0; y < dh; ++y) {
        const AxisSpan& span = ys[static_cast<size_t>(y)];
        for (int x = 0; x < dw; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < span.weights.size(); ++k) {
                const size_t i = pixelIndex(x, span.first + static_cast<int>(k), dw);
                for (int c = 0; c < 4; ++c) {
                    acc[c] += span.weights[k] * mid[i + static_cast<size_t>(c)];
                }
            }
            const size_t o = pixelIndex(x, y, dw);
            const double a = acc[3];
            if (a > 0.0) {
                out.rgba[o] = toByte(acc[0] / a);
                out.rgba[o + 1] = toByte(acc[1] / a);
                out.rgba[o + 2] = toByte(acc[2] / a);
                out.rgba[o + 3] = toByte(a);
            }
        }
    }
    return out;
}

Status crop(const CaptureImage& src, const Rect& rect, CaptureImage& out) {
    if (!(rect.w > 0.0f) || !(rect.h > 0.0f)) {
        return Status::kInvalidArgument;
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(rect.x)), 0, src.width);
    const int y0 = std::clamp(static_cast<int>(std::floor(rect.y)), 0, src.height);
    const int x1 = std::clamp(static_cast<int>(std::ceil(rect.x + rect.w)), 0, src.width);
    const int y1 = std::clamp(static_cast<int>(std::ceil(rect.y + rect.h)), 0, src.height);
    if (x1 <= x0 || y1 <= y0) {
        return Status::kInvalidArgument;
    }
    CaptureImage result;
    result.width = x1 - x0;
    result.height = y1 - y0;
    result.rgba.resize(static_cast<size_t>(result.width) * static_cast<size_t>(result.height) * 4);
    for (int y = 0; y < result.height; ++y) {
        std::copy_n(&src.rgba[pixelIndex(x0, y0 + y, src.width)], static_cast<size_t>(result.width) * 4,
                    &result.rgba[pixelIndex(0, y, result.width)]);
    }
    out = std::move(result);
    return Status::kOk;
}

Status composeSheet(std::span<const CaptureImage> frames, std::span<const std::string> labels, int columns,
                    CaptureImage& out) {
    if (frames.empty() || frames[0].width <= 0 || frames[0].height <= 0) {
        return Status::kInvalidArgument;
    }
    const int cw = frames[0].width, ch = frames[0].height;
    for (const CaptureImage& f : frames) {
        if (f.width != cw || f.height != ch ||
            f.rgba.size() != static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4) {
            return Status::kInvalidArgument;
        }
    }
    const int n = static_cast<int>(frames.size());
    const int cols =
        columns > 0 ? std::min(columns, n) : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const int rows = (n + cols - 1) / cols;
    CaptureImage sheet;
    sheet.width = cols * cw + (cols - 1) * kSheetGap;
    sheet.height = rows * ch + (rows - 1) * kSheetGap;
    sheet.rgba.resize(static_cast<size_t>(sheet.width) * static_cast<size_t>(sheet.height) * 4);
    for (size_t i = 0; i < sheet.rgba.size(); i += 4) {
        sheet.rgba[i] = sheet.rgba[i + 1] = sheet.rgba[i + 2] = 0x80;
        sheet.rgba[i + 3] = 0xFF;
    }
    const int scale = cw >= 256 ? 2 : 1;
    for (int k = 0; k < n; ++k) {
        const int ox = (k % cols) * (cw + kSheetGap);
        const int oy = (k / cols) * (ch + kSheetGap);
        const CaptureImage& f = frames[static_cast<size_t>(k)];
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const size_t s = pixelIndex(x, y, cw);
                const size_t d = pixelIndex(ox + x, oy + y, sheet.width);
                const int a = f.rgba[s + 3];
                for (int c = 0; c < 3; ++c) {
                    const int v = f.rgba[s + static_cast<size_t>(c)] * a + 0x80 * (255 - a);
                    sheet.rgba[d + static_cast<size_t>(c)] = static_cast<uint8_t>((v + 127) / 255);
                }
            }
        }
        if (static_cast<size_t>(k) < labels.size()) {
            drawLabel(sheet, ox + 2, oy + 2, labels[static_cast<size_t>(k)], scale);
        }
    }
    out = std::move(sheet);
    return Status::kOk;
}

int labelWidth(std::string_view text, int scale) {
    const int s = std::max(1, scale);
    return text.empty() ? 0 : static_cast<int>(text.size()) * 6 * s - s;
}

void drawLabel(CaptureImage& dst, int x, int y, std::string_view text, int scale) {
    const int s = std::max(1, scale);
    const int w = labelWidth(text, s);
    const int h = 7 * s;
    if (w <= 0 || dst.width <= 0 || dst.height <= 0) {
        return;
    }
    // The dots as a mask with a one-pixel margin for the border.
    const int mw = w + 2, mh = h + 2;
    std::vector<uint8_t> mask(static_cast<size_t>(mw) * static_cast<size_t>(mh), 0);
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t* rows = glyphRows(text[i]);
        if (rows == nullptr) {
            continue;
        }
        const int gx = 1 + static_cast<int>(i) * 6 * s;
        for (int r = 0; r < 7; ++r) {
            for (int c = 0; c < 5; ++c) {
                if ((rows[r] & (0x10 >> c)) == 0) {
                    continue;
                }
                for (int dy = 0; dy < s; ++dy) {
                    for (int dx = 0; dx < s; ++dx) {
                        mask[static_cast<size_t>(1 + r * s + dy) * static_cast<size_t>(mw) +
                             static_cast<size_t>(gx + c * s + dx)] = 1;
                    }
                }
            }
        }
    }
    for (int my = 0; my < mh; ++my) {
        const int py = y - 1 + my;
        if (py < 0 || py >= dst.height) {
            continue;
        }
        for (int mx = 0; mx < mw; ++mx) {
            const int px = x - 1 + mx;
            if (px < 0 || px >= dst.width) {
                continue;
            }
            uint8_t value = 0;
            bool paint = false;
            if (mask[static_cast<size_t>(my) * static_cast<size_t>(mw) + static_cast<size_t>(mx)] != 0) {
                value = 0xFF;
                paint = true;
            } else {
                for (int ny = std::max(0, my - 1); ny <= std::min(mh - 1, my + 1) && !paint; ++ny) {
                    for (int nx = std::max(0, mx - 1); nx <= std::min(mw - 1, mx + 1); ++nx) {
                        if (mask[static_cast<size_t>(ny) * static_cast<size_t>(mw) +
                                 static_cast<size_t>(nx)] != 0) {
                            paint = true;
                            break;
                        }
                    }
                }
            }
            if (paint) {
                const size_t d = pixelIndex(px, py, dst.width);
                dst.rgba[d] = dst.rgba[d + 1] = dst.rgba[d + 2] = value;
                dst.rgba[d + 3] = 0xFF;
            }
        }
    }
}

void flattenOver(CaptureImage& img, uint8_t r, uint8_t g, uint8_t b) {
    const int bg[3] = {r, g, b};
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const int a = img.rgba[i + 3];
        for (size_t c = 0; c < 3; ++c) {
            const int v = img.rgba[i + c] * a + bg[c] * (255 - a);
            img.rgba[i + c] = static_cast<uint8_t>((v + 127) / 255);
        }
        img.rgba[i + 3] = 0xFF;
    }
}

}  // namespace xt::sw

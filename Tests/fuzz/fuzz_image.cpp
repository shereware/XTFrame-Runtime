#include <cstddef>
#include <cstdint>

#include "sw_dds.h"
#include "xt/asset_resolver.h"
#include "xt/dds.h"
#include "xt/sw/sw_png.h"

/// libFuzzer target: the image decode entry points. The first input byte selects the format
/// (`byte % 2`), the rest is the file:
///   0  DDS: xt::parseDdsHeader (xt_core) and, when it accepts the header, xt::sw::decodeDdsBytes
///      (xt_sw: BC1 / BC3 / BC7 / RGBA8 level 0);
///   1  PNG (and whatever else stb_image takes): xt::sw::decodeImageBytes.
/// On success the decoded size is checked against the pixel buffer. See
/// Docs/design/15-testing.md §15.5, Docs/tasks/T-267b.md.

namespace {
constexpr size_t kMaxInputSize = 1u << 20;  // Docs/tasks/T-39.md "規則".
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > kMaxInputSize) {
        return 0;
    }
    const uint8_t selector = static_cast<uint8_t>(data[0] % 2u);
    const uint8_t* bytes = data + 1;
    const size_t length = size - 1;

    xt::ImageData image;
    bool decoded = false;
    if (selector == 0) {
        xt::DdsInfo info;
        if (!xt::isDds(bytes, length) || !xt::parseDdsHeader(bytes, length, info)) {
            return 0;
        }
        (void)xt::ddsLevelSize(info.format, info.width, info.height);
        decoded = xt::sw::decodeDdsBytes(bytes, length, image);
    } else {
        decoded = xt::sw::decodeImageBytes(bytes, length, image);
    }
    if (!decoded) {
        return 0;
    }

    // A decoder that reports success must hand back exactly width * height RGBA texels.
    const size_t expected = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u;
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() != expected) {
        __builtin_trap();
    }
    return 0;
}

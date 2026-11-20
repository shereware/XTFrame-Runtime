#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/// T-821: the format of an image file told from its leading bytes (never its extension), and
/// whether this build's runtime can decode it. Shared by the importer, the compiler, the packer and
/// the editor so all of them say the same thing about a file the runtime would draw as nothing.
///
/// What the runtime decodes is what `xt::sw::decodeImageBytes` (Code/Runtime/sw/src/sw_image.cpp)
/// decodes -- every renderer gets its pixels through it (FileAssetResolver, AssetResolver):
///  - DDS, by the repository's own reader (xt/dds.h; BC1 / BC3 / BC7 / RGBA8 pixel formats);
///  - everything else by stb_image (Code/SDKs/stb/stb_image.h, compiled once in
///    Code/Runtime/sw/src/sw_png.cpp with no STBI_ONLY_* / STBI_NO_* define), i.e. its full default
///    set: JPEG, PNG, BMP, PSD, TGA, GIF, HDR, PIC, PNM.
/// Keep kDecodableImageFormats and isDecodableImageFormat() in step with those two if either changes.
/// TGA has no signature, so it is never recognised: an unrecognised file is ImageFormat::Unknown,
/// which callers do not report (it may be a TGA, or not an image at all).

namespace xt {

enum class ImageFormat : uint8_t {
    Unknown = 0,
    // Decodable by this build's runtime.
    Png,
    Jpeg,
    Gif,
    Bmp,
    Psd,
    Hdr,
    Pic,
    Pnm,
    Dds,
    // Recognised but not decodable: there is no decoder for these in the runtime.
    WebP,
    Avif,
    Heic,
    Jxl,
    Tiff,
    Qoi,
};

/// How many leading bytes imageFormatOf() looks at at most (fewer are fine: a short file is
/// Unknown unless its signature fits).
inline constexpr size_t kImageFormatHeadBytes = 96;

/// The decodable formats, as listed in messages.
inline constexpr std::string_view kDecodableImageFormats =
    "PNG, JPEG, GIF, BMP, TGA, PSD, HDR, PIC, PNM, DDS";

/// The format `head` (the first `size` bytes of a file) starts with; Unknown when none matches.
ImageFormat imageFormatOf(const uint8_t* head, size_t size);

/// The format's name as messages spell it ("WebP", "PNG"...); "unknown" for Unknown.
std::string_view imageFormatName(ImageFormat format);

/// True for a format the runtime decodes; false for Unknown and the recognised undecodable ones.
bool isDecodableImageFormat(ImageFormat format);

/// True for a recognised format the runtime cannot decode (the case worth a warning).
inline bool isUndecodableImageFormat(ImageFormat format) {
    return format != ImageFormat::Unknown && !isDecodableImageFormat(format);
}

/// `image asset "<name>": format <fmt> cannot be decoded (supported: <kDecodableImageFormats>)` --
/// the compiler's warning, which the editor's lint reports as `asset.undecodable`.
std::string undecodableImageMessage(std::string_view name, ImageFormat format);

/// What the importer and `asset.import` add after the message: the file is copied as it is.
inline constexpr std::string_view kUndecodableImageHint = "convert it to PNG or JPG first";

}  // namespace xt

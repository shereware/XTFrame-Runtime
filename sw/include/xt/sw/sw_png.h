#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"

/// PNG read/write wrapper around stb_image / stb_image_write. See docs/tasks/T-11.md,
/// docs/tasks/T-12.md "AssetResolver".

namespace xt::sw {

/// Writes an RGBA8 buffer (`width` x `height`, row pitch `strideBytes`) to `path` as a PNG.
/// Returns false on failure (null buffer, non-positive dimensions, or a write error). `path` is
/// UTF-8 (T-261e; readPng's too), opened through the wide API on Windows; a path that is not
/// valid UTF-8 is treated as missing (T-261f).
bool writePng(std::string_view path, const uint8_t* rgba, int width, int height, int strideBytes);

/// T-237: encodes an RGBA8 buffer as PNG bytes into `out`, for callers that write the file
/// themselves (e.g. through a UTF-8 path). Returns false on failure, leaving `out` unchanged.
bool encodePng(const uint8_t* rgba, int width, int height, int strideBytes, std::vector<uint8_t>& out);
/// Reads the PNG at `path` into `rgba` as tightly packed RGBA8 (stride == width * 4), setting
/// `width`/`height`. Returns false on failure, leaving `rgba`/`width`/`height` unchanged.
bool readPng(std::string_view path, std::vector<uint8_t>& rgba, int& width, int& height);

/// T-258a: the widest / tallest image decodeImageBytes() decodes (also stb_image's own
/// STBI_MAX_DIMENSIONS, sw_png.cpp). T-258b: public so `xtframe-pack` warns about larger images.
inline constexpr int kMaxImageDimension = 16384;
/// T-258a: the most pixels decodeImageBytes() decodes (64M; 256 MB of RGBA).
inline constexpr int64_t kMaxImagePixels = 67108864;

/// Decodes an in-memory encoded image (any format stb_image supports) to RGBA8, forcing 4
/// channels. Matches xt::ImageDecodeFn's signature; wired into FileAssetResolver by callers (see
/// docs/tasks/T-12.md "AssetResolver"). Returns false on failure, leaving `out` unchanged; an image
/// over kMaxImageDimension per side or kMaxImagePixels in all fails (T-258a).
bool decodeImageBytes(const uint8_t* bytes, size_t size, ImageData& out);

}  // namespace xt::sw

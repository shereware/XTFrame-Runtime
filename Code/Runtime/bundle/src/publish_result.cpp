#include "publish_result.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "xt/asset_resolver.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/utf8_path.h"
#include "xt/dds.h"
#include "xt/sw/sw_png.h"

/// T-714 (Docs/design/22-bundle.md §22.17.2). Not covered by the task sheet (choices made here):
///   - only the 1x file of an image counts (a `@2x` / `@3x` variant replaces it at run time, it is not
///     drawn beside it), and an atlas page counts its 1x file;
///   - SVG images are drawn from their vector source at the size they are shown: not counted;
///   - a file that cannot be read or measured adds to the counts but not to the byte sums.

namespace xt::bundle::detail {

namespace {

namespace fs = std::filesystem;

/// The first `limit` bytes of `path` (fewer for a shorter file); empty when it cannot be read.
std::string readPrefix(const fs::path& path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::string bytes(limit, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(limit));
    bytes.resize(static_cast<size_t>(in.gcount()));
    return bytes;
}

std::string readWhole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())
              : std::string();
}

/// What one bitmap file costs in display memory.
struct ImageCost {
    bool ok = false;
    bool dds = false;
    /// Stored levels for a DDS, else width x height x 4.
    uint64_t gpuBytes = 0;
    /// Logical width x height x 4.
    uint64_t rgbaBytes = 0;
};

ImageCost costOf(const fs::path& path) {
    ImageCost cost;
    // A DDS header is 148 bytes at most (DX10); 64 KiB holds the size of nearly every PNG / JPEG.
    std::string head = readPrefix(path, 64 * 1024);
    const auto* bytes = reinterpret_cast<const uint8_t*>(head.data());
    if (isDds(bytes, head.size())) {
        std::error_code ec;
        const uintmax_t fileSize = fs::file_size(path, ec);
        DdsInfo info;
        // parseDdsHeader wants level 0 in the buffer; only the header fields are used here.
        std::string whole = head.size() < fileSize ? readWhole(path) : head;
        if (!parseDdsHeader(reinterpret_cast<const uint8_t*>(whole.data()), whole.size(), info)) {
            return cost;
        }
        cost.ok = true;
        cost.dds = true;
        for (int level = 0; level < info.mipCount; ++level) {
            cost.gpuBytes += ddsLevelSize(info.format, std::max(1, info.width >> level),
                                          std::max(1, info.height >> level));
        }
        cost.rgbaBytes =
            static_cast<uint64_t>(info.logicalWidth) * static_cast<uint64_t>(info.logicalHeight) * 4;
        return cost;
    }
    int w = 0;
    int h = 0;
    ImageMeasure measured = measureImage(head, compile::pathToUtf8(path.extension()));
    if (!measured.ok) {
        head = readWhole(path);
        measured = measureImage(head, compile::pathToUtf8(path.extension()));
    }
    if (measured.ok) {
        w = measured.w;
        h = measured.h;
    } else {
        ImageData decoded;
        if (head.empty() ||
            !sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(head.data()), head.size(), decoded)) {
            return cost;
        }
        w = decoded.width;
        h = decoded.height;
    }
    cost.ok = true;
    cost.gpuBytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4;
    cost.rgbaBytes = cost.gpuBytes;
    return cost;
}

bool isSvg(const std::string& file) {
    return file.size() >= 4 &&
           (file.compare(file.size() - 4, 4, ".svg") == 0 || file.compare(file.size() - 4, 4, ".SVG") == 0);
}

}  // namespace

void measurePublishResult(const fs::path& outDir, const ManifestData& manifest, PackReport& report) {
    report.imagesCompressed = 0;
    report.imagesUncompressed = 0;
    report.imagesInAtlas = 0;
    report.gpuImageBytes = 0;
    report.gpuImageBytesUncompressed = 0;
    report.bundleBytes = 0;
    for (const ManifestAsset& asset : manifest.assets) {
        if (asset.kind != "image" || isSvg(asset.file)) {
            continue;
        }
        if (!asset.atlases.empty()) {
            ++report.imagesInAtlas;  // Its pixels are counted with the pages below.
            continue;
        }
        const ImageCost cost = costOf(outDir / compile::pathFromUtf8(asset.file));
        if (cost.dds) {
            ++report.imagesCompressed;
        } else {
            ++report.imagesUncompressed;
        }
        report.gpuImageBytes += cost.gpuBytes;
        report.gpuImageBytesUncompressed += cost.rgbaBytes;
    }
    for (const ManifestAtlas& page : manifest.atlases) {
        const ImageCost cost = costOf(outDir / compile::pathFromUtf8(page.file));
        report.gpuImageBytes += cost.gpuBytes;
        report.gpuImageBytesUncompressed += cost.rgbaBytes;
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(outDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec)) {
            it.disable_recursion_pending();  // Links and junctions are not followed.
            continue;
        }
        if (it->is_regular_file(ec)) {
            report.bundleBytes += static_cast<uint64_t>(it->file_size(ec));
        }
    }
}

}  // namespace xt::bundle::detail

namespace xt::bundle {

namespace {

std::string sizeText(uint64_t bytes) {
    char text[48];
    if (bytes >= 1000000) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    } else {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1e3);
    }
    return text;
}

}  // namespace

std::string publishResultSummary(const PackReport& report) {
    return "bundle " + sizeText(report.bundleBytes) +
           " on disk; images: " + std::to_string(report.imagesCompressed) + " compressed, " +
           std::to_string(report.imagesUncompressed) + " uncompressed, " +
           std::to_string(report.imagesInAtlas) + " in atlases; estimated display memory " +
           sizeText(report.gpuImageBytes) + " (" + sizeText(report.gpuImageBytesUncompressed) +
           " uncompressed)";
}

}  // namespace xt::bundle

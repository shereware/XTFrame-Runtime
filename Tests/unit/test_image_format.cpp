#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/image_format.h"

/// T-821 (Docs/tasks/T-821.md): the image format told by the leading bytes (xt/image_format.h) and
/// the compiler's warning for an image asset the runtime cannot decode (a strict compile fails on it,
/// as on the "not found" warning).

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

/// The leading bytes of a WebP file (RIFF container, `WEBP` form, a `VP8 ` chunk header).
const std::string kWebp =
    std::string("RIFF\x1a\x00\x00\x00WEBPVP8 \x0e\x00\x00\x00", 20) + std::string(14, '\0');
const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "fake";
constexpr std::string_view kSupported = "(supported: PNG, JPEG, GIF, BMP, TGA, PSD, HDR, PIC, PNM, DDS)";

ImageFormat formatOf(const std::string& bytes) {
    return imageFormatOf(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

fs::path freshDir(const char* name) {
    const fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::string> warnings(const CompileResult& result) {
    std::vector<std::string> out;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning) {
            out.push_back(d.message);
        }
    }
    return out;
}

bool hasWarning(const CompileResult& result, const std::string& needle) {
    for (const std::string& w : warnings(result)) {
        if (w.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

CompileResult compileOne(const fs::path& root, const std::string& body, const CompileOptions& base = {},
                         const std::string& css = {}) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    SourceFile files[1] = {SourceFile{"main.xt", text}};
    CompileOptions options = base;
    options.rootDir = root.string();
    return compileDocumentFromStrings(files, "main.xt", options);
}

}  // namespace

TEST_CASE("T-821 imageFormatOf: the leading bytes decide; the decodable set is the runtime's") {
    CHECK(formatOf(kPng) == ImageFormat::Png);
    CHECK(formatOf(std::string("\xFF\xD8\xFF\xE0", 4)) == ImageFormat::Jpeg);
    CHECK(formatOf("GIF89a....") == ImageFormat::Gif);
    CHECK(formatOf("GIF87a....") == ImageFormat::Gif);
    CHECK(formatOf("BM\x36\x00\x00\x00") == ImageFormat::Bmp);
    CHECK(formatOf("8BPS\x00\x01") == ImageFormat::Psd);
    CHECK(formatOf("#?RADIANCE\nFORMAT=32-bit_rle_rgbe") == ImageFormat::Hdr);
    CHECK(formatOf("P6\n2 2\n255\n") == ImageFormat::Pnm);
    CHECK(formatOf(std::string("DDS \x7c\x00\x00\x00", 8)) == ImageFormat::Dds);
    std::string pic(92, '\0');
    pic.replace(0, 4, "\x53\x80\xF6\x34");
    pic.replace(88, 4, "PICT");
    CHECK(formatOf(pic) == ImageFormat::Pic);
    for (ImageFormat f :
         {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Gif, ImageFormat::Bmp, ImageFormat::Psd,
          ImageFormat::Hdr, ImageFormat::Pic, ImageFormat::Pnm, ImageFormat::Dds}) {
        CAPTURE(imageFormatName(f));
        CHECK(isDecodableImageFormat(f));
        CHECK_FALSE(isUndecodableImageFormat(f));
    }

    CHECK(formatOf(kWebp) == ImageFormat::WebP);
    CHECK(formatOf(std::string("\x00\x00\x00\x1c"
                               "ftypavif\x00\x00\x00\x00",
                               16)) == ImageFormat::Avif);
    CHECK(formatOf(std::string("\x00\x00\x00\x18"
                               "ftypheic\x00\x00\x00\x00",
                               16)) == ImageFormat::Heic);
    CHECK(formatOf(std::string("\xFF\x0A\xFA\x7F", 4)) == ImageFormat::Jxl);
    CHECK(formatOf(std::string("\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A", 12)) == ImageFormat::Jxl);
    CHECK(formatOf(std::string("II*\x00\x08\x00\x00\x00", 8)) == ImageFormat::Tiff);
    CHECK(formatOf(std::string("MM\x00*\x00\x00\x00\x08", 8)) == ImageFormat::Tiff);
    CHECK(formatOf("qoif\x00\x00\x00\x02") == ImageFormat::Qoi);
    for (ImageFormat f : {ImageFormat::WebP, ImageFormat::Avif, ImageFormat::Heic, ImageFormat::Jxl,
                          ImageFormat::Tiff, ImageFormat::Qoi}) {
        CAPTURE(imageFormatName(f));
        CHECK_FALSE(isDecodableImageFormat(f));
        CHECK(isUndecodableImageFormat(f));
    }

    // Unknown is neither: TGA has no signature, and a file that is no image at all says nothing.
    CHECK(formatOf(std::string("\x00\x00\x02\x00\x00\x00\x00\x00", 8)) == ImageFormat::Unknown);  // a TGA
    CHECK(formatOf("not an image") == ImageFormat::Unknown);
    CHECK(formatOf(std::string("RIFF\x1a\x00\x00\x00", 8)) == ImageFormat::Unknown);  // cut short
    CHECK(formatOf("") == ImageFormat::Unknown);
    CHECK(imageFormatOf(nullptr, 16) == ImageFormat::Unknown);
    CHECK_FALSE(isDecodableImageFormat(ImageFormat::Unknown));
    CHECK_FALSE(isUndecodableImageFormat(ImageFormat::Unknown));

    CHECK(imageFormatName(ImageFormat::WebP) == "WebP");
    CHECK(undecodableImageMessage("img/fox.webp", ImageFormat::WebP) ==
          "image asset \"img/fox.webp\": format WebP cannot be decoded " + std::string(kSupported));
}

TEST_CASE("T-821 compile: an undecodable <img src> / background-image / particle texture is a warning") {
    const fs::path root = freshDir("xt_t821_compile");
    writeBytes(root / "fox.webp", kWebp);
    writeBytes(root / "disguised.png", kWebp);  // the bytes decide, not the name
    writeBytes(root / "spark.webp", kWebp);
    writeBytes(root / "real.webp", kPng);
    writeBytes(root / "plain.png", kPng);
    const std::string body =
        "<img src=\"fox.webp\"/><div id=\"bg\"></div><particles texture=\"spark.webp\"></particles>"
        "<img src=\"real.webp\"/><img src=\"plain.png\"/>";
    const CompileResult result =
        compileOne(root, body, {}, "#bg { background-image: url(\"disguised.png\"); }");
    for (const std::string& w : warnings(result)) {
        CAPTURE(w);
    }
    CHECK(result.ok());
    CHECK(hasWarning(result,
                     "image asset \"fox.webp\": format WebP cannot be decoded " + std::string(kSupported)));
    CHECK(hasWarning(result, "image asset \"disguised.png\": format WebP cannot be decoded"));
    CHECK(hasWarning(result, "image asset \"spark.webp\": format WebP cannot be decoded"));
    CHECK_FALSE(hasWarning(result, "real.webp"));
    CHECK_FALSE(hasWarning(result, "plain.png"));

    CompileOptions strict;
    strict.strict = true;
    CHECK_FALSE(compileOne(root, "<img src=\"fox.webp\"/>", strict).ok());
    CHECK(compileOne(root, "<img src=\"plain.png\"/>", strict).ok());

    CompileOptions embed;
    embed.embedImages = true;
    CHECK(hasWarning(compileOne(root, "<img src=\"fox.webp\"/>", embed),
                     "image asset \"fox.webp\": format WebP cannot be decoded"));
}

TEST_CASE("T-821 compile: readAssetHead answers instead of the disk") {
    const fs::path root = freshDir("xt_t821_hooks");  // empty: nothing below exists on disk
    std::vector<std::string> asked;
    CompileOptions options;
    options.fileExists = [](std::string_view) { return true; };
    options.readAssetHead = [&](std::string_view path, std::vector<uint8_t>& out) {
        asked.emplace_back(path);
        out.assign(kWebp.begin(), kWebp.end());
        return true;
    };
    const CompileResult result = compileOne(root, "<img src=\"img/a.png\"/>", options);
    CHECK(hasWarning(result, "image asset \"img/a.png\": format WebP cannot be decoded"));
    REQUIRE(asked.size() == 1);
    CHECK(fs::path(asked[0]) == root / "img" / "a.png");
}

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "doctest.h"
#include "t687_project.h"
#include "test_temp.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/project_edit.h"
#include "xt/bundle/ui_project.h"

/// T-714 (Docs/design/22-bundle.md §22.17, Docs/tasks/T-714.md): the publish entry points resolve
/// an unset `publish.images.compress` to `auto` through effectivePublishCompress() while pack()'s own
/// default stays `none`; PackReport carries the publish result (bundle size, image counts, estimated
/// display memory); the publish settings of the Publish dialog and `project.setPublishSettings`
/// (publishSettings / validatePublishSettings / setPublishSettings) write the `.xtui`.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;
namespace t687 = xt::test::t687;

std::string readAllT714(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// The T-687 project (bg 512 x 288, art 302 x 300, wide 300 x 255, icon 64 x 64) with `publishJson`.
std::string projectT714(const std::string& name, const std::string& publishJson) {
    const fs::path dir = xt::test::tempDir() / ("xt_t714_src_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    REQUIRE(t687::writeProject(dir, publishJson));
    return dir.generic_string();
}

UiProject loadT714(const std::string& dir) {
    std::string error;
    Result<UiProject> project = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(project.ok(), error);
    return std::move(project.value());
}

PackReport packT714(const UiProject& project, const std::string& outName, PackOptions options) {
    const fs::path out = xt::test::tempDir() / ("xt_t714_out_" + outName);
    std::error_code ec;
    fs::remove_all(out, ec);
    options.outDir = out.generic_string();
    options.thumbnails = false;
    options.encodeCache = false;
    PackReport report = pack(project, options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    return report;
}

/// Every file under `assets/` and `atlas/`, and `manifest.json` without its `builtAt` line.
std::map<std::string, std::string> stableT714(const std::string& outName) {
    const fs::path dir = xt::test::tempDir() / ("xt_t714_out_" + outName);
    std::map<std::string, std::string> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = fs::relative(entry.path(), dir).generic_string();
        std::string bytes = readAllT714(entry.path());
        if (name == "manifest.json") {
            const size_t at = bytes.find("\"builtAt\"");
            if (at != std::string::npos) {
                bytes.erase(at, bytes.find('\n', at) - at);
            }
            files[name] = bytes;
        } else if (name.rfind("assets/", 0) == 0 || name.rfind("atlas/", 0) == 0) {
            files[name] = bytes;
        }
    }
    return files;
}

uint64_t treeBytes(const std::string& outName) {
    const fs::path dir = xt::test::tempDir() / ("xt_t714_out_" + outName);
    uint64_t total = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            total += static_cast<uint64_t>(entry.file_size());
        }
    }
    return total;
}

}  // namespace

TEST_CASE("T-714: effectivePublishCompress -- unset is auto, a written value is kept") {
    UiProject project;
    CHECK(effectivePublishCompress(project) == "auto");
    for (const char* value : {"none", "bc7", "auto"}) {
        CAPTURE(value);
        UiProject written = loadT714(projectT714(
            std::string("eff_") + value, std::string(R"({ "images": { "compress": ")") + value + "\" } }"));
        CHECK(written.publish.images.present);
        CHECK(effectivePublishCompress(written) == value);
    }
    // No `publish.images` in the file: auto (and pack()'s own default is still none, below).
    UiProject absent = loadT714(projectT714("eff_absent", ""));
    CHECK_FALSE(absent.publish.images.present);
    CHECK(absent.publish.images.compress == "none");
    CHECK(effectivePublishCompress(absent) == "auto");
    // A value set in memory is what save() writes back, so it is the effective one too.
    absent.publish.images.compress = "bc7";
    CHECK(effectivePublishCompress(absent) == "bc7");
}

TEST_CASE("T-810: a publish.images object without compress is auto, and saving does not add compress") {
    // 22 §22.17.1 point 3: what decides is whether `compress` is written, not whether `images` is.
    for (const char* images : {R"({ "images": {} })", R"({ "images": { "keep": 1 } })"}) {
        CAPTURE(images);
        const std::string dir = projectT714("t810_nokey", images);
        UiProject project = loadT714(dir);
        CHECK(project.publish.images.present);
        CHECK(effectivePublishCompress(project) == "auto");
        CHECK(publishSettings(project).images.compress == "none");  // unwritten (none = the field's default)
        // Round trip: the file did not have `compress`, the saved one does not either.
        std::string error;
        REQUIRE_MESSAGE(project.save(&error) == Status::kOk, error);
        // The file save() wrote: the project is named "T687", so it is T687.xtui (the fixture's t687.xtui
        // is replaced, T-214) -- the same file only on a case-insensitive file system.
        const std::string text = readAllT714(fs::path(project.path()));
        CHECK(text.find("\"compress\"") == std::string::npos);
        CHECK(text.find("\"images\"") != std::string::npos);
        const UiProject reread = loadT714(dir);
        CHECK(reread.publish.images.present);
        CHECK(effectivePublishCompress(reread) == "auto");
        // A settings round trip that leaves the image setting alone does not write it either.
        UiProject edited = loadT714(dir);
        PublishSettings s = publishSettings(edited);
        s.atlas.enabled = true;
        REQUIRE(setPublishSettings(edited, s, &error) == Status::kOk);
        CHECK(readAllT714(fs::path(edited.path())).find("\"compress\"") == std::string::npos);
        CHECK(effectivePublishCompress(loadT714(dir)) == "auto");
    }
    // Written values next to other keys are kept as they are (including none), and saved back.
    for (const char* value : {"none", "bc7", "auto"}) {
        CAPTURE(value);
        const std::string dir =
            projectT714(std::string("t810_key_") + value,
                        std::string(R"({ "images": { "keep": 1, "compress": ")") + value + "\" } }");
        UiProject project = loadT714(dir);
        CHECK(effectivePublishCompress(project) == value);
        std::string error;
        REQUIRE_MESSAGE(project.save(&error) == Status::kOk, error);
        CHECK(effectivePublishCompress(loadT714(dir)) == value);
    }
}

TEST_CASE("T-714: pack() with the default PackOptions is unchanged (none); the entry point default is auto") {
    const UiProject absent = loadT714(projectT714("lib_absent", ""));
    const PackReport plain = packT714(absent, "lib_default", PackOptions{});
    REQUIRE(plain.ok);
    CHECK(plain.compress == "none");
    CHECK(plain.imagesCompressed == 0);
    PackOptions none;
    none.compress = "none";
    REQUIRE(packT714(absent, "lib_none", none).ok);
    CHECK(stableT714("lib_default") == stableT714("lib_none"));
    for (const auto& [file, bytes] : stableT714("lib_default")) {
        CHECK(file.find(".dds") == std::string::npos);
    }
    // What a publish entry point passes.
    PackOptions entry;
    entry.compress = effectivePublishCompress(absent);
    const PackReport published = packT714(absent, "lib_entry", entry);
    REQUIRE(published.ok);
    CHECK(published.compress == "auto");
    CHECK(published.imagesCompressed == 2);
}

TEST_CASE("T-714: PackReport's publish result, hand-computed for the T-687 project") {
    const UiProject project = loadT714(projectT714("report", ""));
    PackOptions options;
    options.compress = "auto";
    const PackReport report = packT714(project, "report_auto", options);
    REQUIRE(report.ok);
    CHECK(report.imagesCompressed == 2);    // bg, art
    CHECK(report.imagesUncompressed == 2);  // wide, icon
    CHECK(report.imagesInAtlas == 0);
    // bg 512 x 288, BC7 (16 B per 4 x 4 block), 10 levels:
    //   128*72 + 64*36 + 32*18 + 16*9 + 8*5 + 4*3 + 2*1 + 1 + 1 + 1 blocks = 12297 blocks = 196752 B.
    // art 302 x 300 padded to 304 x 300, 9 levels:
    //   76*75 + 38*38 + 19*19 + 10*10 + 5*5 + 3*3 + 1 + 1 + 1 blocks = 7642 blocks = 122272 B.
    // wide 300 x 255 and icon 64 x 64 as RGBA8: 306000 + 16384 B.
    CHECK(report.gpuImageBytes == 196752u + 122272u + 306000u + 16384u);
    // Uncompressed, at the logical sizes: 512*288*4 + 302*300*4 + 306000 + 16384.
    CHECK(report.gpuImageBytesUncompressed == 589824u + 362400u + 306000u + 16384u);
    CHECK(report.bundleBytes == treeBytes("report_auto"));
    CHECK(report.bundleBytes > 0);

    // Under none every image is RGBA8: both sums are the uncompressed one.
    options.compress = "none";
    const PackReport none = packT714(project, "report_none", options);
    REQUIRE(none.ok);
    CHECK(none.imagesCompressed == 0);
    CHECK(none.imagesUncompressed == 4);
    CHECK(none.gpuImageBytes == none.gpuImageBytesUncompressed);
    CHECK(none.gpuImageBytes == 589824u + 362400u + 306000u + 16384u);
    CHECK(none.bundleBytes == treeBytes("report_none"));
}

TEST_CASE("T-714: images in atlases are counted apart; the pages carry the display memory") {
    const UiProject project = loadT714(projectT714("atlas", ""));
    PackOptions options;
    options.compress = "none";
    options.atlas = true;
    options.atlasScope = "bundle";
    const PackReport report = packT714(project, "atlas_none", options);
    REQUIRE(report.ok);
    REQUIRE(report.atlasPages > 0);
    CHECK(report.imagesInAtlas == report.atlasEntries);
    CHECK(report.imagesInAtlas + report.imagesUncompressed + report.imagesCompressed == 4);
    // Pages and the images kept as files, all RGBA8: compressed and uncompressed sums agree, and the
    // pages hold at least the pixels of their entries.
    CHECK(report.gpuImageBytes == report.gpuImageBytesUncompressed);
    CHECK(report.gpuImageBytes >= report.atlasPixelsTotal * 4);
}

TEST_CASE("T-714: publishResultSummary is one line with the size, the counts and the display memory") {
    PackReport report;
    report.bundleBytes = 12'345'678;
    report.imagesCompressed = 3;
    report.imagesUncompressed = 2;
    report.imagesInAtlas = 1;
    report.gpuImageBytes = 5'100'000;
    report.gpuImageBytesUncompressed = 21'000'000;
    CHECK(publishResultSummary(report) ==
          "bundle 12.3 MB on disk; images: 3 compressed, 2 uncompressed, 1 in atlases; estimated display "
          "memory 5.1 MB (21.0 MB uncompressed)");
    report.bundleBytes = 2'048;
    report.gpuImageBytes = 0;
    report.gpuImageBytesUncompressed = 999'949;
    CHECK(publishResultSummary(report) ==
          "bundle 2.0 KB on disk; images: 3 compressed, 2 uncompressed, 1 in atlases; estimated display "
          "memory 0.0 KB (999.9 KB uncompressed)");
}

TEST_CASE("T-714: setPublishSettings writes the publish fields, keeps the others and unknown keys") {
    const std::string dir = projectT714(
        "settings",
        R"({ "outDir": "out", "languages": ["en"], "atlas": { "padding": 5 }, "images": { "compress": "none", "keep": 1 }, "custom": true })");
    UiProject project = loadT714(dir);
    PublishSettings s = publishSettings(project);
    CHECK(s.images.compress == "none");
    CHECK(s.atlas.padding == 5);
    s.images.compress = "bc7";
    s.atlas.enabled = true;
    s.atlas.maxSize = 2048;
    s.atlas.scope = "bundle";
    s.atlas.mipmaps = true;
    s.atlas.padding = 0;  // not one of the dialog's fields: ignored
    s.fonts = FontsMode::kBundled;
    s.embedImages = true;
    s.embedFonts = true;
    std::string error;
    REQUIRE(setPublishSettings(project, s, &error) == Status::kOk);
    const UiProject reread = loadT714(dir);
    CHECK(reread.publish.images.compress == "bc7");
    CHECK(reread.publish.atlas.enabled);
    CHECK(reread.publish.atlas.maxSize == 2048);
    CHECK(reread.publish.atlas.scope == "bundle");
    CHECK(reread.publish.atlas.mipmaps);
    CHECK(reread.publish.atlas.padding == 5);
    CHECK(reread.publish.fonts == FontsMode::kBundled);
    CHECK(reread.publish.embedImages);
    CHECK(reread.publish.embedFonts);
    CHECK(reread.publish.outDir == "out");
    CHECK(reread.publish.languages == std::vector<std::string>{"en"});
    const std::string text = readAllT714(fs::path(reread.path()));  // T687.xtui, see the T-810 case
    CHECK(text.find("\"keep\"") != std::string::npos);              // unknown key of publish.images
    CHECK(text.find("\"custom\"") != std::string::npos);            // unknown key of publish

    // Invalid values: nothing changes.
    PublishSettings bad = publishSettings(reread);
    bad.images.compress = "bc3";
    CHECK(setPublishSettings(project, bad, &error) == Status::kInvalidArgument);
    CHECK(error.find("images.compress") != std::string::npos);
    bad = publishSettings(reread);
    bad.atlas.maxSize = 1000;
    CHECK(setPublishSettings(project, bad, &error) == Status::kInvalidArgument);
    bad = publishSettings(reread);
    bad.atlas.scope = "page";
    CHECK(setPublishSettings(project, bad, &error) == Status::kInvalidArgument);
    CHECK(project.publish.images.compress == "bc7");
    CHECK(project.publish.atlas.maxSize == 2048);
}

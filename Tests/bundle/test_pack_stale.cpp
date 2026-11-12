#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "stale_files.h"
#include "t687_project.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/bundle/bundle.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"

/// T-693 (Docs/tasks/T-693.md): a successful pack removes the files earlier packs left in the
/// directories it manages -- the PNGs a `bc7` pack replaced with DDS files and back, an older image of
/// a project -- but never another project's files that the directory's manifest lists, nothing at the
/// root it does not know, nothing on a failed run or under `keepStale`, and nothing through a link or
/// junction. A stale same-hash PNG beside its DDS changes nothing when the bundle is drawn.

using namespace xt;
using namespace xt::bundle;

namespace {

namespace fs = std::filesystem;
namespace t687 = xt::test::t687;

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Every regular file below `dir` (relative generic path -> bytes).
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            files[fs::relative(entry.path(), dir).generic_string()] = readAll(entry.path());
        }
    }
    return files;
}

/// tree() with what differs from run to run blanked: `ui/*.xtir` carry their compile time (META),
/// and the manifest's `builtAt` line.
std::map<std::string, std::string> stableTree(const fs::path& dir) {
    std::map<std::string, std::string> files = tree(dir);
    for (auto& [name, bytes] : files) {
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".xtir") == 0) {
            bytes = "<xtir>";
        } else if (name == "manifest.json") {
            std::string kept;
            size_t start = 0;
            while (start < bytes.size()) {
                size_t end = bytes.find('\n', start);
                end = end == std::string::npos ? bytes.size() : end + 1;
                const std::string line = bytes.substr(start, end - start);
                if (line.find("\"builtAt\"") == std::string::npos) {
                    kept += line;
                }
                start = end;
            }
            bytes = kept;
        }
    }
    return files;
}

fs::path freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_t693_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

PackReport packProject(const fs::path& source, const fs::path& out, PackOptions options) {
    std::string error;
    Result<UiProject> project = UiProject::load(source.generic_string(), &error);
    REQUIRE_MESSAGE(project.ok(), error);
    options.outDir = out.generic_string();
    PackReport report = pack(project.value(), options);
    for (const compile::Diagnostic& d : report.diagnostics) {
        MESSAGE(d.file, ": ", d.message);
    }
    return report;
}

PackOptions compressed(const std::string& format) {
    PackOptions options;
    options.thumbnails = false;
    options.compress = format;
    return options;
}

/// A one-UI project `id` whose UI `ui` (64 x 64) draws `img/pic.png`, a flat `rgb` square.
fs::path miniProject(const std::string& name, const std::string& id, const std::string& ui, uint8_t r,
                     uint8_t g, uint8_t b) {
    const fs::path dir = freshDir("src_" + name);
    writeAll(dir / (id + ".xtui"), R"({ "version": 1, "name": ")" + id + R"(", "id": ")" + id +
                                       R"(", "uis": [ { "id": ")" + ui + R"(", "doc": ")" + ui + "/" + ui +
                                       R"(.xt", "size": [64, 64] } ] })" + "\n");
    writeAll(
        dir / ui / (ui + ".xt"),
        "<xt>\n<head><style>\n#p { position: absolute; left: 0px; top: 0px; width: 64px; height: 64px; }\n"
        "</style></head>\n<body>\n<img id=\"p\" src=\"img/pic.png\"/>\n</body>\n</xt>\n");
    std::error_code ec;
    fs::create_directories(dir / ui / "img", ec);
    REQUIRE(t687::writeImage(dir / ui / "img" / "pic.png", t687::flat(64, 64, r, g, b, 255)));
    return dir;
}

/// The files of `files` that `prefix` starts.
std::set<std::string> namesUnder(const std::map<std::string, std::string>& files, const std::string& prefix) {
    std::set<std::string> names;
    for (const auto& [name, bytes] : files) {
        if (name.rfind(prefix, 0) == 0) {
            names.insert(name);
        }
    }
    return names;
}

/// A FileAssetResolver that records every name it reads.
class CountingResolver : public FileAssetResolver {
public:
    using FileAssetResolver::FileAssetResolver;
    bool readBytes(std::string_view name, std::vector<uint8_t>& out) override {
        names.emplace_back(name);
        return FileAssetResolver::readBytes(name, out);
    }
    std::vector<std::string> names;
};

/// Draws the bundle's UI `m` (640 x 320, SW) with `resolver` supplying every asset.
std::vector<uint8_t> drawThrough(Bundle& bundle, AssetResolver& resolver) {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &resolver});
    Result<DocumentHandle> handle = loadUi(ctx, bundle, "m");
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 640, 320}, 1.0f);
    ctx.update(0.0f);
    std::vector<uint8_t> pixels(static_cast<size_t>(640) * 320 * 4, 0);
    sw::SwTarget target{pixels.data(), 640, 320, 640 * 4};
    CommandList list;
    ctx.render(list);
    sw::SwRenderer renderer;
    renderer.setTarget(target);
    renderer.clear(Color{0.5f, 0.5f, 0.5f, 1});
    renderer.render(list, &shaper, &resolver, &ctx.document(handle.value())->ir(), nullptr);
    return pixels;
}

std::unique_ptr<Bundle> openOrFail(const fs::path& dir) {
    std::string error;
    Result<std::unique_ptr<Bundle>> bundle = Bundle::open(dir.generic_string(), sw::decodeImageBytes, &error);
    REQUIRE_MESSAGE(bundle.ok(), error);
    return std::move(bundle.value());
}

}  // namespace

TEST_CASE("T-693: none -> bc7 into one directory removes the PNGs and equals a fresh bc7 pack; and back") {
    const fs::path source = freshDir("src_switch");
    REQUIRE(t687::writeProject(source, ""));
    const fs::path out = freshDir("out_switch");
    const fs::path freshBc7 = freshDir("out_fresh_bc7");
    const fs::path freshNone = freshDir("out_fresh_none");

    const PackReport first = packProject(source, out, compressed("none"));
    REQUIRE(first.ok);
    CHECK(first.staleFilesRemoved == 0);
    const std::map<std::string, std::string> afterNone = stableTree(out);
    // With nothing stale a repeated pack removes nothing and writes the same bundle.
    const PackReport again = packProject(source, out, compressed("none"));
    REQUIRE(again.ok);
    CHECK(again.staleFilesRemoved == 0);
    CHECK(stableTree(out) == afterNone);

    const PackReport toBc7 = packProject(source, out, compressed("bc7"));
    REQUIRE(toBc7.ok);
    REQUIRE(packProject(source, freshBc7, compressed("bc7")).ok);
    const std::map<std::string, std::string> bc7Files = stableTree(out);
    CHECK(bc7Files == stableTree(freshBc7));
    for (const std::string& name : namesUnder(bc7Files, "assets/")) {
        CHECK_MESSAGE(name.find(".png") == std::string::npos, name);
    }
    CHECK(toBc7.staleFilesRemoved == namesUnder(afterNone, "assets/").size());
    CHECK(toBc7.staleBytesRemoved > 0);

    const PackReport back = packProject(source, out, compressed("none"));
    REQUIRE(back.ok);
    REQUIRE(packProject(source, freshNone, compressed("none")).ok);
    CHECK(stableTree(out) == stableTree(freshNone));
    CHECK(back.staleFilesRemoved == namesUnder(bc7Files, "assets/").size());
}

TEST_CASE(
    "T-693: two projects packed into one directory keep each other's files; a re-pack removes only its own") {
    const fs::path alpha = miniProject("alpha", "alpha", "a", 200, 30, 30);
    const fs::path beta = miniProject("beta", "beta", "b", 30, 30, 200);
    const fs::path out = freshDir("out_shared");
    PackOptions options;  // Thumbnails on: `thumbs/` is covered as well.

    REQUIRE(packProject(alpha, out, options).ok);
    const std::map<std::string, std::string> alphaFiles = tree(out);
    REQUIRE(alphaFiles.count("thumbs/a.png") == 1);
    REQUIRE(alphaFiles.count("ui/a.xtir") == 1);

    const PackReport second = packProject(beta, out, options);
    REQUIRE(second.ok);
    CHECK(second.staleFilesRemoved == 0);
    const std::map<std::string, std::string> afterBeta = tree(out);
    for (const auto& [name, bytes] : alphaFiles) {
        if (name != "manifest.json") {
            CHECK_MESSAGE(afterBeta.count(name) == 1, name);
        }
    }
    const std::set<std::string> alphaAssets = namesUnder(alphaFiles, "assets/");
    REQUIRE(alphaAssets.size() == 1);
    const std::set<std::string> betaAssets = namesUnder(afterBeta, "assets/");
    REQUIRE(betaAssets.size() == 2);

    // alpha's picture changes: its old asset goes, beta's files (the manifest's) all stay.
    REQUIRE(t687::writeImage(alpha / "a" / "img" / "pic.png", t687::flat(64, 64, 10, 220, 10, 255)));
    const PackReport third = packProject(alpha, out, options);
    REQUIRE(third.ok);
    CHECK(third.staleFilesRemoved == 1);
    const std::map<std::string, std::string> afterAlpha = tree(out);
    CHECK(afterAlpha.count(*alphaAssets.begin()) == 0);
    for (const auto& [name, bytes] : afterBeta) {
        if (name != "manifest.json" && alphaFiles.count(name) == 0) {
            CHECK_MESSAGE(afterAlpha.count(name) == 1, name);  // beta's assets, ui/b.xtir, thumbs/b.png
        }
    }
    CHECK(afterAlpha.count("ui/a.xtir") == 1);
    CHECK(afterAlpha.count("thumbs/a.png") == 1);
    CHECK(namesUnder(afterAlpha, "assets/").size() == 2);
}

TEST_CASE("T-693: unknown root files and directories stay; known root leftovers and stale managed files go") {
    const fs::path source = miniProject("root", "rooty", "r", 90, 90, 90);
    const fs::path out = freshDir("out_root");
    REQUIRE(packProject(source, out, PackOptions{}).ok);
    writeAll(out / "notes.txt", "mine");
    writeAll(out / "extra" / "keep.png", "mine too");
    writeAll(out / "assets" / "nested" / "deep.png", "below a managed directory");
    writeAll(out / "manifest.json.tmp", "left by a killed write");
    writeAll(out / "bundle.xtflow", "{}");
    writeAll(out / "bundle.xtflow.tmp", "left by a killed write");
    writeAll(out / "assets" / "0123456789abcdef.png", "stale");
    writeAll(out / "ui" / "gone.xtir", "stale");
    writeAll(out / "fonts" / "old.subset.ttf", "stale");

    const PackReport report = packProject(source, out, PackOptions{});
    REQUIRE(report.ok);
    // manifest.json.tmp is consumed by the run's own atomic manifest write; the other five are removed.
    CHECK(report.staleFilesRemoved == 5);
    CHECK(readAll(out / "notes.txt") == "mine");
    CHECK(readAll(out / "extra" / "keep.png") == "mine too");
    CHECK(fs::exists(out / "assets" / "nested" / "deep.png"));
    CHECK_FALSE(fs::exists(out / "manifest.json.tmp"));
    CHECK_FALSE(fs::exists(out / "bundle.xtflow"));
    CHECK_FALSE(fs::exists(out / "bundle.xtflow.tmp"));
    CHECK_FALSE(fs::exists(out / "assets" / "0123456789abcdef.png"));
    CHECK_FALSE(fs::exists(out / "ui" / "gone.xtir"));
    CHECK_FALSE(fs::exists(out / "fonts" / "old.subset.ttf"));
    CHECK(fs::exists(out / "ui" / "r.xtir"));
}

TEST_CASE("T-693: a failed pack and --keep-stale remove nothing; an unreadable earlier manifest keeps all") {
    const fs::path source = miniProject("fail", "failing", "f", 50, 60, 70);
    const fs::path out = freshDir("out_fail");
    REQUIRE(packProject(source, out, PackOptions{}).ok);
    writeAll(out / "assets" / "0123456789abcdef.png", "stale");

    // keepStale: the stale file stays.
    PackOptions keep;
    keep.keepStale = true;
    const PackReport kept = packProject(source, out, keep);
    REQUIRE(kept.ok);
    CHECK(kept.staleFilesRemoved == 0);
    CHECK(fs::exists(out / "assets" / "0123456789abcdef.png"));

    // A failure before anything is written leaves the directory exactly as it was.
    const std::map<std::string, std::string> before = tree(out);
    PackOptions bogus;
    bogus.compress = "bogus";
    CHECK_FALSE(packProject(source, out, bogus).ok);
    CHECK(tree(out) == before);

    // A UI that does not compile: the run fails and removes nothing.
    writeAll(source / "f" / "f.xt", "<xt>\n<body>\n<div id=\"x\">\n</body>\n</xt>\n<<<\n");
    const PackReport failed = packProject(source, out, PackOptions{});
    REQUIRE_FALSE(failed.ok);
    CHECK(failed.staleFilesRemoved == 0);
    for (const auto& [name, bytes] : before) {
        CHECK_MESSAGE(fs::exists(out / name), name);
    }

    // An earlier manifest.json that cannot be read: whose files are there is unknown, nothing goes.
    const fs::path good = miniProject("fail2", "failing", "f", 50, 60, 70);
    writeAll(out / "manifest.json", "not json");
    const PackReport unread = packProject(good, out, PackOptions{});
    REQUIRE(unread.ok);
    CHECK(unread.staleFilesRemoved == 0);
    CHECK(fs::exists(out / "assets" / "0123456789abcdef.png"));
    // The next run finds its own manifest and cleans.
    const PackReport next = packProject(good, out, PackOptions{});
    REQUIRE(next.ok);
    CHECK(next.staleFilesRemoved == 1);
    CHECK_FALSE(fs::exists(out / "assets" / "0123456789abcdef.png"));
}

TEST_CASE("T-693: an encode cache inside the bundle is left alone") {
    const fs::path source = freshDir("src_cache");
    REQUIRE(t687::writeProject(source, ""));
    const fs::path out = freshDir("out_cache");
    REQUIRE(packProject(source, out, compressed("none")).ok);
    const size_t pngs = namesUnder(tree(out), "assets/").size();

    // `--cache-dir <out>/assets`: that directory is not managed, its PNGs and cache entries stay.
    PackOptions intoAssets = compressed("bc7");
    intoAssets.encodeCacheDir = (out / "assets").generic_string();
    const PackReport report = packProject(source, out, intoAssets);
    REQUIRE(report.ok);
    CHECK(report.staleFilesRemoved == 0);
    size_t cacheEntries = 0;
    size_t stalePngs = 0;
    for (const std::string& name : namesUnder(tree(out), "assets/")) {
        cacheEntries += name.find(".xtbc") != std::string::npos ? 1u : 0u;
        stalePngs += name.find(".png") != std::string::npos ? 1u : 0u;
    }
    CHECK(cacheEntries > 0);
    CHECK(stalePngs == pngs);

    // `--cache-dir <out>/.cache`: an unknown subdirectory, never looked into.
    PackOptions intoOwn = compressed("bc7");
    intoOwn.encodeCacheDir = (out / ".cache").generic_string();
    const PackReport own = packProject(source, out, intoOwn);
    REQUIRE(own.ok);
    CHECK(own.staleFilesRemoved == pngs + cacheEntries);  // Now `assets/` is managed again.
    CHECK_FALSE(namesUnder(tree(out), ".cache/").empty());
}

TEST_CASE("T-693: removeStaleFiles never follows a link or junction out of the bundle") {
    const fs::path out = freshDir("out_links");
    const fs::path outside = freshDir("outside");
    writeAll(outside / "victim.png", "outside the bundle");
    writeAll(outside / "linked.png", "a file a link inside the bundle names");
    std::error_code ec;
    fs::create_directories(out / "assets", ec);

    bool dirLink = false;
    fs::create_directory_symlink(outside, out / "thumbs", ec);
    dirLink = !ec;
#if defined(_WIN32)
    if (!dirLink) {
        // A junction needs no privilege.
        const std::string command = "cmd /c mklink /J \"" +
                                    fs::path(out / "thumbs").make_preferred().string() + "\" \"" +
                                    fs::path(outside).make_preferred().string() + "\" >NUL";
        dirLink = std::system(command.c_str()) == 0 && fs::exists(out / "thumbs" / "victim.png");
    }
#endif
    ec.clear();
    fs::create_symlink(outside / "linked.png", out / "assets" / "linked.png", ec);
    const bool fileLink = !ec;
    MESSAGE("directory link: ", dirLink, ", file link: ", fileLink);

    xt::bundle::detail::KeptBundleFiles keep;
    const xt::bundle::detail::StaleRemoval removed =
        xt::bundle::detail::removeStaleFiles(out, keep, fs::path());
    CHECK(removed.files == 0);
    CHECK(readAll(outside / "victim.png") == "outside the bundle");
    CHECK(readAll(outside / "linked.png") == "a file a link inside the bundle names");
    if (fileLink) {
        CHECK(fs::is_symlink(out / "assets" / "linked.png"));
    }
    fs::remove(out / "thumbs", ec);  // The link / junction only, not what it points at.
    CHECK(fs::exists(outside / "victim.png"));
}

TEST_CASE("T-693: a stale same-hash PNG beside its DDS changes neither the pixels nor the files read") {
    const fs::path source = freshDir("src_load");
    REQUIRE(t687::writeProject(source, ""));
    const fs::path stale = freshDir("out_load_stale");
    const fs::path clean = freshDir("out_load_clean");
    REQUIRE(packProject(source, stale, compressed("none")).ok);
    PackOptions keepBc7 = compressed("bc7");
    keepBc7.keepStale = true;
    REQUIRE(packProject(source, stale, keepBc7).ok);
    REQUIRE(packProject(source, clean, compressed("bc7")).ok);

    // The stale directory holds `<hash>.png` beside `<hash>.dds` for every compressed image.
    const std::map<std::string, std::string> staleFiles = tree(stale);
    size_t pairs = 0;
    for (const std::string& name : namesUnder(staleFiles, "assets/")) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dds") == 0) {
            pairs += staleFiles.count(name.substr(0, name.size() - 4) + ".png");
        }
    }
    CHECK(pairs == 4);

    std::unique_ptr<Bundle> staleBundle = openOrFail(stale);
    std::unique_ptr<Bundle> cleanBundle = openOrFail(clean);
    CountingResolver staleReads(stale.generic_string(), sw::decodeImageBytes);
    CountingResolver cleanReads(clean.generic_string(), sw::decodeImageBytes);
    const std::vector<uint8_t> stalePixels = drawThrough(*staleBundle, staleReads);
    const std::vector<uint8_t> cleanPixels = drawThrough(*cleanBundle, cleanReads);
    CHECK(stalePixels == cleanPixels);
    CHECK(staleReads.names == cleanReads.names);
    CHECK(staleReads.names.size() == cleanReads.names.size());
    MESSAGE("files read: ", staleReads.names.size());
    std::set<std::string> listed;
    for (const AssetInfo& info : staleBundle->assetTable()) {
        listed.insert(info.file);
    }
    for (const std::string& name : staleReads.names) {
        CAPTURE(name);
        CHECK(listed.count(name) == 1);
        CHECK(name.find(".png") == std::string::npos);
    }
    CHECK_FALSE(staleReads.names.empty());
}

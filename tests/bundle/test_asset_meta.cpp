#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/asset_meta.h"
#include "xt/bundle/design_doc.h"
#include "xt/bundle/design_system.h"
#include "xt/bundle/pack.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/utf8_path.h"
#include "xt/sw/sw_png.h"

/// Asset identity, the import pipeline, source folders, the generated asset manifest, the cleanup
/// numbers and promoted references (docs/design/35-design-system.md §35.15, 40 §40.2.1;
/// docs/tasks/T-312a.md item 8).

using namespace xt;
using namespace xt::bundle;
using namespace xt::bundle::assets;

namespace {

namespace fs = std::filesystem;
using compile::pathFromUtf8;

std::string sourceDir(const char* relative) { return std::string(XT_TEST_BUNDLE_DIR) + "/../../" + relative; }

std::string freshDir(const std::string& name) {
    const fs::path dir = xt::test::tempDir() / ("xt_assetmeta_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.generic_string();
}

std::string readText(const std::string& path) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(pathFromUtf8(path).parent_path(), ec);
    std::ofstream out(pathFromUtf8(path), std::ios::binary);
    out << text;
}

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(pathFromUtf8(path), ec);
}

/// A `w` x `h` PNG: a solid colour, or (noise) pseudo-random pixels.
void writePng(const std::string& path, int w, int h, uint32_t rgb, bool noise = false) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    uint32_t state = rgb | 1u;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (noise) {
            state = state * 1664525u + 1013904223u;
            rgba[i] = static_cast<uint8_t>(state >> 24);
            rgba[i + 1] = static_cast<uint8_t>(state >> 16);
            rgba[i + 2] = static_cast<uint8_t>(state >> 8);
        } else {
            rgba[i] = static_cast<uint8_t>(rgb >> 16);
            rgba[i + 1] = static_cast<uint8_t>(rgb >> 8);
            rgba[i + 2] = static_cast<uint8_t>(rgb);
        }
        rgba[i + 3] = 255;
    }
    std::error_code ec;
    fs::create_directories(pathFromUtf8(path).parent_path(), ec);
    REQUIRE(sw::writePng(path, rgba.data(), w, h, w * 4));
}

/// A blank UI Project (createUiProject(): `main/main.xt`, `shared/`, `design/`).
UiProject makeProject(const std::string& name) {
    const std::string dir = freshDir(name) + "/project";
    UiProject project;
    std::string error;
    REQUIRE_MESSAGE(createUiProject(dir, "Asset Test", "asset-test", "", &project, &error) == Status::kOk,
                    error);
    return project;
}

std::string abs(const UiProject& p, const std::string& rel) { return p.rootDir + "/" + rel; }

size_t at(const std::string& text, const std::string& key) { return text.find("\n" + key + ":"); }

bool hasDiagnostic(const PackReport& report, const std::string& needle, compile::Diagnostic::Level level) {
    for (const compile::Diagnostic& d : report.diagnostics) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("T-312a (a): meta text is byte-stable, in the fixed key order, unknown keys kept") {
    AssetMeta meta;
    meta.id = "8f3a0000-0000-4000-8000-000000000001";
    meta.type = "image";
    meta.title = "Star icon";
    meta.collection = "icons";
    meta.contentHash = "sha256:abc";
    meta.status = "approved";
    meta.license = "own";
    meta.source = design::Source{"import:figma", "12:34", "2026-10-11T08:00:00Z"};
    meta.approved = design::Approval{"lead", "2026-10-11"};
    meta.tags = {"hud", "icon"};
    meta.measured.hasSize = true;
    meta.measured.w = 32;
    meta.measured.h = 16;
    meta.measured.hasAlpha = true;
    meta.measured.alpha = true;
    meta.import = nlohmann::json::object();
    meta.import["slice"] = nlohmann::json::array({4, 4, 2, 2});
    meta.import["scale2x"] = true;
    meta.links.push_back(design::Link{"derived-from", "ref:moodboard/a", ""});
    meta.note = "hand-drawn";
    meta.extra = nlohmann::json::object();
    meta.extra["zzz"] = "kept";
    meta.extra["derived"] = nlohmann::json::array();

    const std::string text = metaText(meta);
    AssetMeta back;
    std::string error;
    REQUIRE_MESSAGE(parseMeta(text, back, &error), error);
    CHECK(metaText(back) == text);
    CHECK(back.id == meta.id);
    CHECK(back.source.ref == "12:34");
    CHECK(back.measured.w == 32);
    CHECK(back.import["slice"][2] == 2);
    REQUIRE(back.links.size() == 1);
    CHECK(back.links[0].to == "ref:moodboard/a");
    CHECK(back.extra["zzz"] == "kept");
    // Fixed order: version (T-406), id, type, title, collection, status, source, approved, license,
    // content, tags, measured, import, links, note, then the unknown keys alphabetically.
    CHECK(text.rfind("version: 1\nid: ", 0) == 0);
    const char* order[] = {"type",     "title",   "collection", "status",  "source",
                           "approved", "license", "content",    "tags",    "measured",
                           "import",   "links",   "note",       "derived", "zzz"};
    size_t last = 0;
    for (const char* key : order) {
        const size_t pos = at(text, key);
        CHECK_MESSAGE(pos != std::string::npos, key);
        CHECK_MESSAGE(pos > last, key);
        last = pos;
    }
    CHECK(text.find('\r') == std::string::npos);

    // writeMeta / readMeta round trip.
    const std::string dir = freshDir("meta_io");
    writeText(dir + "/star.png", "x");
    REQUIRE(writeMeta(dir + "/star.png", meta) == Status::kOk);
    CHECK(readText(dir + "/star.png.meta") == text);
    AssetMeta read;
    REQUIRE(readMeta(dir + "/star.png", read) == Status::kOk);
    CHECK(metaText(read) == text);
    CHECK(readMeta(dir + "/none.png", read) == Status::kNotFound);
    // A minimal meta: status is always written; nothing empty is.
    AssetMeta minimal;
    CHECK(metaText(minimal) == "version: 1\nstatus: draft\n");
}

TEST_CASE("T-312a (b): ensureMeta, collections and import paths") {
    UiProject project = makeProject("ensure");
    writePng(abs(project, "shared/img/icons/star.png"), 8, 4, 0x336699);
    AssetMeta meta;
    bool created = false;
    REQUIRE(ensureMeta(project, "shared/img/icons/star.png", meta, &created) == Status::kOk);
    CHECK(created);
    CHECK(meta.id.size() == 36);
    CHECK(meta.type == "image");
    CHECK(meta.collection == "icons");
    CHECK(meta.contentHash.rfind("sha256:", 0) == 0);
    CHECK(meta.measured.hasSize);
    CHECK(meta.measured.w == 8);
    CHECK(meta.measured.h == 4);
    CHECK(meta.source.kind == "manual");
    CHECK(meta.status == "draft");
    CHECK(exists(abs(project, "shared/img/icons/star.png.meta")));
    const std::string id = meta.id;
    AssetMeta again;
    REQUIRE(ensureMeta(project, "shared/img/icons/star.png", again, &created) == Status::kOk);
    CHECK_FALSE(created);
    CHECK(again.id == id);
    // The file changed outside: same identity, new hash and size.
    writePng(abs(project, "shared/img/icons/star.png"), 16, 4, 0x336699);
    REQUIRE(ensureMeta(project, "shared/img/icons/star.png", again, &created) == Status::kOk);
    CHECK(again.id == id);
    CHECK(again.contentHash != meta.contentHash);
    CHECK(again.measured.w == 16);
    CHECK(ensureMeta(project, "shared/img/icons/missing.png", again, nullptr) == Status::kNotFound);

    CHECK(collectionOf(project, "shared/img/icons/star.png") == "icons");
    CHECK(collectionOf(project, "shared/img/hud/frames/a.png") == "hud/frames");
    CHECK(collectionOf(project, "shared/img/a.png").empty());
    CHECK(collectionOf(project, "main/a.png").empty());
    CHECK(importPathFor(project, "image", "icons", "My Icon.PNG") == "shared/img/icons/my-icon.png");
    CHECK(importPathFor(project, "image", "", "Star@2x.png") == "shared/img/star@2x.png");
    CHECK(importPathFor(project, "font", "ui", "Noto.ttf") == "shared/fonts/ui/noto.ttf");
    CHECK(importPathFor(project, "anim", "fx/hits", "boom.json") == "shared/anim/fx/hits/boom.json");
    CHECK(importPathFor(project, "image", "../out", "a.png").empty());
    CHECK(importPathFor(project, "image", "/abs", "a.png").empty());
    CHECK(importPathFor(project, "image", "a\\b", "a.png").empty());
    CHECK(typeForExtension(".SVG") == "vector");
    CHECK(typeForExtension(".xtfx") == "effect");
    CHECK(typeForExtension(".txt").empty());
    CHECK(kebabAssetId("My Logo") == "my-logo");
}

TEST_CASE("T-312a (c): importAsset copies, numbers same names, deduplicates, reads @2x") {
    UiProject project = makeProject("import");
    const std::string src = freshDir("import_src");
    writePng(src + "/A.png", 4, 4, 0x112233);
    writePng(src + "/other/A.png", 4, 4, 0x445566);
    writePng(src + "/b.png", 4, 4, 0x112233);  // same content as A.png
    writePng(src + "/A@2x.png", 8, 8, 0x112233);
    writeText(src + "/notes.txt", "not an asset");

    ImportRequest request;
    request.sourcePath = src + "/A.png";
    request.collection = "ui";
    request.title = "Button A";
    request.tags = {"button"};
    ImportResult result;
    REQUIRE(importAsset(project, request, result) == Status::kOk);
    CHECK_FALSE(result.deduplicated);
    CHECK(result.relFile == "shared/img/ui/a.png");
    CHECK(exists(abs(project, "shared/img/ui/a.png")));
    CHECK(exists(abs(project, "shared/img/ui/a.png.meta")));
    CHECK(result.meta.title == "Button A");
    CHECK(result.meta.collection == "ui");
    CHECK(result.meta.status == "draft");
    CHECK(result.meta.source.kind == "manual");
    CHECK(result.meta.tags == std::vector<std::string>{"button"});

    // Same name, other content: `-2`.
    request.sourcePath = src + "/other/A.png";
    REQUIRE(importAsset(project, request, result) == Status::kOk);
    CHECK(result.relFile == "shared/img/ui/a-2.png");

    // Same content: nothing copied.
    request.sourcePath = src + "/b.png";
    REQUIRE(importAsset(project, request, result) == Status::kOk);
    CHECK(result.deduplicated);
    CHECK(result.duplicateOf == "shared/img/ui/a.png");
    CHECK(result.relFile == "shared/img/ui/a.png");
    CHECK_FALSE(result.warnings.empty());
    CHECK_FALSE(exists(abs(project, "shared/img/ui/b.png")));

    // @2x from the file name.
    request.sourcePath = src + "/A@2x.png";
    REQUIRE(importAsset(project, request, result) == Status::kOk);
    CHECK(result.relFile == "shared/img/ui/a@2x.png");
    CHECK(result.meta.import["scale2x"] == true);

    // Batch: the second of two equal files is deduplicated against the first.
    writePng(src + "/c1.png", 2, 2, 0x778899);
    writePng(src + "/c2.png", 2, 2, 0x778899);
    std::vector<ImportRequest> batch(2);
    batch[0].sourcePath = src + "/c1.png";
    batch[1].sourcePath = src + "/c2.png";
    std::vector<ImportResult> results;
    REQUIRE(importAssets(project, batch, results) == Status::kOk);
    REQUIRE(results.size() == 2);
    CHECK(results[0].relFile == "shared/img/c1.png");
    CHECK(results[1].deduplicated);
    CHECK(results[1].duplicateOf == "shared/img/c1.png");

    request.sourcePath = src + "/notes.txt";
    CHECK(importAsset(project, request, result) == Status::kUnsupported);
    request.sourcePath = src + "/A.png";
    request.collection = "../escape";
    CHECK(importAsset(project, request, result) == Status::kInvalidArgument);
    request.collection = "ui";
    request.status = "shipped";
    CHECK(importAsset(project, request, result) == Status::kInvalidArgument);
    request.status = "draft";
    request.sourcePath = src + "/missing.png";
    CHECK(importAsset(project, request, result) == Status::kNotFound);
}

TEST_CASE("T-312a (d): replaceAsset keeps the identity") {
    UiProject project = makeProject("replace");
    writePng(abs(project, "shared/img/hud/bar.png"), 4, 4, 0x101010);
    AssetMeta meta;
    REQUIRE(ensureMeta(project, "shared/img/hud/bar.png", meta, nullptr) == Status::kOk);
    meta.status = "approved";
    meta.title = "Bar";
    REQUIRE(writeMeta(abs(project, "shared/img/hud/bar.png"), meta) == Status::kOk);
    const std::string src = freshDir("replace_src");
    writePng(src + "/new.png", 6, 4, 0x202020);
    AssetMeta replaced;
    REQUIRE(replaceAsset(project, "shared/img/hud/bar.png", src + "/new.png", replaced) == Status::kOk);
    CHECK(replaced.id == meta.id);
    CHECK(replaced.title == "Bar");
    CHECK(replaced.contentHash != meta.contentHash);
    CHECK(replaced.measured.w == 6);
    CHECK(replaced.status == "draft");
    CHECK_FALSE(replaced.source.at.empty());
    CHECK(readText(abs(project, "shared/img/hud/bar.png")) == readText(src + "/new.png"));
    writeText(src + "/new.svg", "<svg width=\"4\" height=\"4\"></svg>");
    CHECK(replaceAsset(project, "shared/img/hud/bar.png", src + "/new.svg", replaced) ==
          Status::kInvalidArgument);
    CHECK(replaceAsset(project, "shared/img/hud/none.png", src + "/new.png", replaced) == Status::kNotFound);
}

TEST_CASE("T-312a (e): source folders: new, changed, missing; repeatable; stored in the .xtui") {
    UiProject project = makeProject("sync");
    const std::string folder = freshDir("sync_src");
    writePng(folder + "/Icon One.png", 4, 4, 0x010203);
    writePng(folder + "/two.png", 4, 4, 0x040506);
    writeText(folder + "/readme.txt", "ignored");
    AssetSourceFolder linked;
    linked.path = folder;
    linked.collection = "export";
    linked.importJson = "{\"compress\":\"bc7\"}";
    project.assets.sourceFolders.push_back(linked);
    project.spec.budgetAssetBytes = 4096;
    REQUIRE(project.save() == Status::kOk);
    std::string error;
    Result<UiProject> loaded = UiProject::load(project.rootDir, &error);
    REQUIRE_MESSAGE(loaded.ok(), error);
    REQUIRE(loaded.value().assets.sourceFolders.size() == 1);
    CHECK(loaded.value().assets.sourceFolders[0].path == folder);
    CHECK(loaded.value().assets.sourceFolders[0].collection == "export");
    CHECK(loaded.value().spec.budgetAssetBytes == 4096);
    const std::string xtui = readText(project.path());
    REQUIRE(loaded.value().save() == Status::kOk);
    CHECK(readText(project.path()) == xtui);  // byte-stable
    project = loaded.value();
    REQUIRE(sourceFolders(project).size() == 1);
    CHECK(sourceFolders(project)[0].import["compress"] == "bc7");

    SyncReport report;
    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.imported ==
          std::vector<std::string>{"shared/img/export/icon-one.png", "shared/img/export/two.png"});
    CHECK(report.replaced.empty());
    AssetMeta meta;
    REQUIRE(readMeta(abs(project, "shared/img/export/two.png"), meta) == Status::kOk);
    CHECK(meta.source.kind == "import:folder");
    CHECK(meta.source.ref == folder + "/two.png");
    CHECK(meta.import["compress"] == "bc7");
    const std::string id = meta.id;

    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.imported.empty());
    CHECK(report.replaced.empty());
    CHECK(report.unchanged.size() == 2);

    writePng(folder + "/two.png", 4, 4, 0x070809);
    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.replaced == std::vector<std::string>{"shared/img/export/two.png"});
    REQUIRE(readMeta(abs(project, "shared/img/export/two.png"), meta) == Status::kOk);
    CHECK(meta.status == "stale");
    CHECK(meta.id == id);
    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.replaced.empty());
    CHECK(report.unchanged.size() == 2);

    std::error_code ec;
    fs::remove(pathFromUtf8(folder + "/two.png"), ec);
    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.missing == std::vector<std::string>{"shared/img/export/two.png"});
    CHECK(exists(abs(project, "shared/img/export/two.png")));  // never deleted
    REQUIRE(syncSourceFolders(project, report) == Status::kOk);
    CHECK(report.missing == std::vector<std::string>{"shared/img/export/two.png"});
    CHECK(report.unchanged == std::vector<std::string>{"shared/img/export/icon-one.png"});
}

TEST_CASE(
    "T-312a (f): regenerateAssetsManifest keeps hand-written entries; fixtures without metas unchanged") {
    // Projects without design/ and without any .meta are not upgraded: xtframe-pack leaves them alone.
    {
        const std::string dir = freshDir("regen_fixture");
        std::error_code ec;
        fs::copy(sourceDir("tests/bundle/fixtures/sample"), dir, fs::copy_options::recursive, ec);
        REQUIRE(ec.value() == 0);
        Result<UiProject> sample = UiProject::load(dir);
        REQUIRE(sample.ok());
        CHECK_FALSE(assetsUpgraded(sample.value()));
        const std::string game = freshDir("regen_game_plain");
        fs::copy(sourceDir("examples/demos/game-ui"), game, fs::copy_options::recursive, ec);
        REQUIRE(ec.value() == 0);
        Result<UiProject> gameUi = UiProject::load(game);
        REQUIRE(gameUi.ok());
        CHECK_FALSE(assetsUpgraded(gameUi.value()));
    }

    UiProject project = makeProject("regen");
    CHECK(assetsUpgraded(project));  // createUiProject() made design/
    writePng(abs(project, "shared/img/frame.png"), 16, 16, 0x0a0a0a);
    writePng(abs(project, "shared/img/frame@2x.png"), 32, 32, 0x0a0a0a);
    writePng(abs(project, "shared/img/icons/frame.png"), 8, 8, 0x0b0b0b);
    writeText(abs(project, "shared/anim/pulse.json"), "{\"v\":\"5.7.0\",\"layers\":[]}");
    writeText(abs(project, kAssetsManifestPath),
              "version: 1\n"
              "assets:\n"
              "  - id: panel-frame\n"
              "    type: image\n"
              "    file: shared/img/frame.png\n"
              "    source: external:store\n"
              "    status: approved\n"
              "    license: CC0\n"
              "    measured: { slice: [4, 4, 4, 4] }\n"
              "    compress: bc3\n"
              "    owner: art\n"
              "  - id: gone\n"
              "    type: image\n"
              "    file: shared/img/gone.png\n");
    AssetsManifest manifest;
    RegenerateReport report;
    std::string error;
    REQUIRE_MESSAGE(regenerateAssetsManifest(project, manifest, &report, &error) == Status::kOk, error);
    CHECK(report.written);
    CHECK(report.metasCreated == 3);
    REQUIRE(manifest.assets.size() == 4);
    // Generated in path order, then the unmatched hand-written entry.
    CHECK(manifest.assets[0].file == "shared/anim/pulse.json");
    CHECK(manifest.assets[0].id == "pulse");
    CHECK(manifest.assets[0].type == "anim");
    CHECK(manifest.assets[1].file == "shared/img/frame.png");
    CHECK(manifest.assets[1].id == "panel-frame");
    CHECK(manifest.assets[1].source == "external:store");
    CHECK(manifest.assets[1].status == "approved");
    CHECK(manifest.assets[1].license == "CC0");
    CHECK(manifest.assets[1].measured.hasSlice);
    CHECK(manifest.assets[1].measured.w == 16);
    CHECK(manifest.assets[2].file == "shared/img/icons/frame.png");
    CHECK(manifest.assets[2].id == "frame");
    CHECK(manifest.assets[3].id == "gone");
    bool owner = false;
    bool compress = false;
    for (const auto& [key, value] : manifest.assets[1].extra) {
        owner = owner || (key == "owner" && value == "art");
        compress = compress || (key == "compress" && value == "bc3");
    }
    CHECK(owner);
    CHECK(compress);
    // The meta was seeded from the hand-written entry.
    AssetMeta meta;
    REQUIRE(readMeta(abs(project, "shared/img/frame.png"), meta) == Status::kOk);
    CHECK(meta.status == "approved");
    CHECK(meta.license == "CC0");
    CHECK(meta.source.kind == "external:store");
    CHECK(meta.import["slice"][0] == 4);
    CHECK(meta.import["compress"] == "bc3");
    CHECK_FALSE(exists(abs(project, "shared/img/frame@2x.png.meta")));  // a variant
    // Same inputs, same bytes: not written again.
    const std::string first = readText(abs(project, kAssetsManifestPath));
    REQUIRE(regenerateAssetsManifest(project, manifest, &report, &error) == Status::kOk);
    CHECK_FALSE(report.written);
    CHECK(report.metasCreated == 0);
    CHECK(readText(abs(project, kAssetsManifestPath)) == first);
    // checkAssets() is unchanged and still reports the missing hand-written file.
    bool filled = false;
    const std::vector<AssetIssue> issues = checkAssets(manifest, project.rootDir, nullptr, &filled);
    bool gone = false;
    for (const AssetIssue& issue : issues) {
        gone = gone || (issue.id == "gone" && issue.message.find("not found") != std::string::npos);
        if (issue.id != "gone") {
            CHECK_MESSAGE(!issue.isError, (issue.id + ": " + issue.message));
        }
    }
    CHECK(gone);
    // A meta status change flows into the manifest.
    meta.status = "pending";
    REQUIRE(writeMeta(abs(project, "shared/img/frame.png"), meta) == Status::kOk);
    REQUIRE(regenerateAssetsManifest(project, manifest, &report, &error) == Status::kOk);
    CHECK(report.written);
    CHECK(manifest.assets[1].status == "pending");
    // A manifest with comments is not rewritten.
    const std::string current = readText(abs(project, kAssetsManifestPath));
    writeText(abs(project, kAssetsManifestPath), "# mine\n" + current);
    REQUIRE(regenerateAssetsManifest(project, manifest, &report, &error) == Status::kOk);
    CHECK_FALSE(report.written);
    CHECK(report.warnings.size() == 1);
    // A malformed one is an error.
    writeText(abs(project, kAssetsManifestPath), "assets: 3\n");
    CHECK(regenerateAssetsManifest(project, manifest, &report, &error) == Status::kInvalidArgument);
}

TEST_CASE("T-312a (g): regenerateAssetsManifest on game-ui (timing)") {
    const std::string dir = freshDir("regen_game");
    std::error_code ec;
    fs::copy(sourceDir("examples/demos/game-ui"), dir, fs::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    std::string error;
    Result<UiProject> game = UiProject::load(dir, &error);
    REQUIRE_MESSAGE(game.ok(), error);
    REQUIRE(design::upgradeDesign(game.value(), nullptr, &error) == Status::kOk);
    CHECK(assetsUpgraded(game.value()));
    AssetsManifest manifest;
    RegenerateReport report;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(regenerateAssetsManifest(game.value(), manifest, &report, &error) == Status::kOk, error);
    const double coldMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(report.metasCreated == 2);
    const auto again = std::chrono::steady_clock::now();
    REQUIRE(regenerateAssetsManifest(game.value(), manifest, &report, &error) == Status::kOk);
    const double warmMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - again).count();
    CHECK(manifest.assets.size() == 2);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "T-312a regenerateAssetsManifest: game-ui %zu entries, first run %.2f ms (metas created), "
                  "second run %.2f ms",
                  manifest.assets.size(), coldMs, warmMs);
    MESSAGE(line);
    std::printf("%s\n", line);
}

TEST_CASE("T-312a (h): assetHygiene numbers") {
    UiProject project = makeProject("hygiene");
    project.spec.budgetAssetBytes = 4000;
    writePng(abs(project, "shared/img/ui/used.png"), 8, 8, 0x111111);
    writePng(abs(project, "shared/img/ui/dup.png"), 8, 8, 0x111111);
    writePng(abs(project, "shared/img/ui/unused.png"), 8, 8, 0x222222);
    writePng(abs(project, "shared/img/ui/big.png"), 128, 128, 0x333333, true);
    writePng(abs(project, "shared/img/ui/frame.png"), 8, 8, 0x444444);
    writePng(abs(project, "shared/img/ui/Bad Name.png"), 8, 8, 0x555555);
    writePng(abs(project, "shared/img/ui/temp.png"), 8, 8, 0x666666);
    writePng(abs(project, "shared/img/hd/a.png"), 8, 8, 0x777777);
    writePng(abs(project, "shared/img/hd/a@2x.png"), 16, 16, 0x777777);
    writePng(abs(project, "shared/img/hd/b.png"), 8, 8, 0x888888);
    writeText(abs(project, "main/main.xt"),
              "<body><img src=\"../shared/img/ui/used.png\"/><img src=\"../shared/img/ui/dup.png\"/>"
              "<img src=\"../shared/img/ui/big.png\"/><img src=\"../shared/img/ui/frame.png\"/>"
              "<img src=\"../shared/img/ui/Bad Name.png\"/><img src=\"../shared/img/ui/temp.png\"/>"
              "<img src=\"../shared/img/hd/a.png\"/><img src=\"../shared/img/hd/b.png\"/></body>\n");
    AssetMeta meta;
    REQUIRE(ensureMeta(project, "shared/img/ui/frame.png", meta, nullptr) == Status::kOk);
    meta.import = nlohmann::json::object();
    meta.import["slice"] = nlohmann::json::array({4, 4, 1, 1});  // no stretchable centre in 8 px
    REQUIRE(writeMeta(abs(project, "shared/img/ui/frame.png"), meta) == Status::kOk);
    REQUIRE(ensureMeta(project, "shared/img/ui/temp.png", meta, nullptr) == Status::kOk);
    meta.status = "placeholder";
    REQUIRE(writeMeta(abs(project, "shared/img/ui/temp.png"), meta) == Status::kOk);

    AssetHygiene h;
    REQUIRE(assetHygiene(project, nullptr, h) == Status::kOk);
    CHECK(h.totalAssets == 9);  // a@2x.png is a variant
    CHECK(h.unused == 1);
    CHECK(h.unusedFiles == std::vector<std::string>{"shared/img/ui/unused.png"});
    CHECK(h.duplicates == 1);
    REQUIRE(h.duplicatePairs.size() == 1);
    CHECK(h.duplicatePairs[0].first == "shared/img/ui/dup.png");
    CHECK(h.duplicatePairs[0].second == "shared/img/ui/used.png");
    CHECK(h.oversize == 1);
    CHECK(h.oversizeFiles == std::vector<std::string>{"shared/img/ui/big.png"});
    CHECK(h.missing2x == 1);
    CHECK(h.badSlice == 1);
    CHECK(h.badName == 1);
    CHECK(h.placeholders == 1);
    CHECK(h.placeholderFiles == std::vector<std::string>{"shared/img/ui/temp.png"});
    REQUIRE(h.bytesPerUi.count("main") == 1);
    CHECK(h.bytesPerUi["main"] > 4000);
    REQUIRE(h.collections.size() == 2);
    CHECK(h.collections[0].collection == "hd");
    CHECK(h.collections[0].assets == 2);
    CHECK(h.collections[1].collection == "ui");
    CHECK(h.collections[1].assets == 7);
    // Nothing was written (frame / temp had metas already).
    CHECK_FALSE(exists(abs(project, "shared/img/ui/used.png.meta")));
    // No budget: oversize not checked.
    project.spec.budgetAssetBytes = 0;
    REQUIRE(assetHygiene(project, nullptr, h) == Status::kOk);
    CHECK(h.oversize == 0);

    // The document's section 13 shows the numbers; the lint reports the placeholder.
    design::DesignSystem ds;
    design::Graph graph;
    REQUIRE(design::load(project, ds) == Status::kOk);
    REQUIRE(design::buildGraph(project, ds, graph) == Status::kOk);
    std::string md;
    REQUIRE(design::buildDocument(project, ds, graph, design::BuildOptions(), md, nullptr) == Status::kOk);
    CHECK(md.find("| `img/ui` | 7 |") != std::string::npos);
    CHECK(md.find("- Unused: 1") != std::string::npos);
    CHECK(md.find("- Duplicates: 1") != std::string::npos);
    CHECK(md.find("- Missing @2x: 1") != std::string::npos);
    CHECK(md.find("- Bad nine-slice: 1") != std::string::npos);
    CHECK(md.find("- Placeholders: 1") != std::string::npos);
    CHECK(md.find("### Per UI") != std::string::npos);
    bool placeholderIssue = false;
    for (const design::Issue& issue : design::lintDocument(project, ds, graph)) {
        placeholderIssue = placeholderIssue || (issue.id == "asset.placeholderShipped" &&
                                                issue.severity == design::Severity::Error &&
                                                issue.entry == "shared/img/ui/temp.png");
    }
    CHECK(placeholderIssue);
}

TEST_CASE("T-312a (i): promoteReference whole and cropped; pack refuses placeholders") {
    UiProject project = makeProject("promote");
    // A 16 x 8 reference: left half red, right half blue.
    std::vector<uint8_t> rgba(16 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint8_t* p = &rgba[(static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4];
            p[0] = x < 8 ? 255 : 0;
            p[1] = 0;
            p[2] = x < 8 ? 0 : 255;
            p[3] = 255;
        }
    }
    const std::string refFile = design::designDir(project) + "/references/moodboard/key.png";
    std::error_code ec;
    fs::create_directories(pathFromUtf8(refFile).parent_path(), ec);
    REQUIRE(sw::writePng(refFile, rgba.data(), 16, 8, 16 * 4));
    writeText(refFile + ".meta",
              "id: 11111111-2222-4333-8444-555555555555\nkind: reference\ntitle: Key art\nlicense: "
              "internal-only\n");
    writeText(design::designDir(project) + "/references/notes.md", "# notes\n");
    writeText(design::designDir(project) + "/references/notes.md.meta", "kind: reference\n");

    ImportResult whole;
    REQUIRE(promoteReference(project, "moodboard/key", nullptr, "refs", whole) == Status::kOk);
    CHECK(whole.relFile == "shared/img/refs/key.png");
    CHECK(whole.meta.status == "placeholder");
    CHECK(whole.meta.license == "internal-only");
    CHECK(whole.meta.title == "Key art");
    REQUIRE(whole.meta.links.size() == 1);
    CHECK(whole.meta.links[0].rel == "derived-from");
    CHECK(whole.meta.links[0].to == "ref:moodboard/key");
    CHECK(whole.meta.measured.w == 16);

    const std::array<float, 4> left = {0.0f, 0.0f, 0.5f, 1.0f};
    ImportResult cropped;
    REQUIRE(promoteReference(project, "ref:moodboard/key", &left, "refs", cropped) == Status::kOk);
    CHECK(cropped.relFile == "shared/img/refs/key-2.png");
    CHECK(cropped.meta.status == "placeholder");
    CHECK(cropped.meta.measured.w == 8);
    CHECK(cropped.meta.measured.h == 8);
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    REQUIRE(sw::readPng(abs(project, cropped.relFile), pixels, w, h));
    REQUIRE(w == 8);
    CHECK(pixels[0] == 255);
    CHECK(pixels[2] == 0);
    CHECK(pixels[(7 * 4) + 2] == 0);  // still red at the right edge of the crop

    const std::array<float, 4> empty = {0.5f, 0.5f, 0.0f, 0.0f};
    CHECK(promoteReference(project, "moodboard/key", &empty, "refs", cropped) == Status::kInvalidArgument);
    CHECK(promoteReference(project, "moodboard/none", nullptr, "refs", cropped) == Status::kNotFound);
    CHECK(promoteReference(project, "notes", nullptr, "refs", cropped) == Status::kUnsupported);

    // The pack refuses a placeholder unless --allow-placeholders.
    AssetsManifest manifest;
    std::string error;
    REQUIRE_MESSAGE(regenerateAssetsManifest(project, manifest, nullptr, &error) == Status::kOk, error);
    REQUIRE(manifest.assets.size() == 2);
    PackOptions options;
    options.outDir = freshDir("promote_out");
    options.thumbnails = false;
    const PackReport refused = pack(project, options);
    CHECK_FALSE(refused.ok);
    CHECK(hasDiagnostic(refused, "asset.placeholderShipped", compile::Diagnostic::Level::Error));
    CHECK_FALSE(hasDiagnostic(refused, "status 'placeholder'", compile::Diagnostic::Level::Error));
    options.allowPlaceholders = true;
    const PackReport allowed = pack(project, options);
    for (const compile::Diagnostic& d : allowed.diagnostics) {
        CHECK_MESSAGE(d.level != compile::Diagnostic::Level::Error, d.message);
    }
    CHECK(allowed.ok);
    CHECK(hasDiagnostic(allowed, "asset.placeholderShipped", compile::Diagnostic::Level::Warning));
}

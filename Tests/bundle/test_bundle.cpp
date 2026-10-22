#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/bundle/bundle.h"
#include "xt/ir_pod.h"

/// Bundle: reading a published bundle's `manifest.json`, its port contract, its assets and the
/// hot-reload diff. See Docs/design/22-bundle.md §22.3/§22.5, Docs/tasks/T-90.md "test_bundle".

using namespace xt;
using namespace xt::bundle;

namespace {

std::string fixture(const std::string& name) { return std::string(XT_TEST_BUNDLE_DIR) + "/fixtures/" + name; }

/// T-262a: a manifest with one nine-slice asset (`nine`) and one atlas cell at (`cellX`, 0).
std::string metaManifest(const std::string& nine, int cellX) {
    return "{\"version\": 1, \"bundle\": {\"id\": \"mygame\", \"irMajor\": " +
           std::to_string(static_cast<int>(kIrFormatMajor)) + "}, \"uis\": [], \"assets\": [{\"file\": \"" +
           nine +
           "\", \"kind\": \"image\", \"manifest\": {\"slice\": [8, 8, 4, 4]}}, "
           "{\"file\": \"assets/cell.png\", \"kind\": \"image\", \"atlas\": [\"0\"]}], "
           "\"atlas\": [{\"name\": \"0\", \"file\": \"atlas/0.png\", \"w\": 64, \"h\": 64, \"entries\": "
           "[{\"asset\": \"assets/cell.png\", \"x\": " +
           std::to_string(cellX) + ", \"y\": 0, \"w\": 16, \"h\": 16}]}]}";
}

/// A fresh copy of the published fixture under the system temp dir.
std::string copiedFixture(const std::string& name) {
    std::filesystem::path dir = xt::test::tempDir() / ("xt_bundle_" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::copy(fixture("published"), dir, std::filesystem::copy_options::recursive, ec);
    REQUIRE(ec.value() == 0);
    return dir.generic_string();
}

void writeManifest(const std::string& dir, const std::string& text) {
    std::ofstream out(dir + "/manifest.json", std::ios::binary);
    out << text;
}

const PortInfo* findPort(const UiInfo& ui, const std::string& name) {
    for (const PortInfo& port : ui.ports) {
        if (port.name == name) {
            return &port;
        }
    }
    return nullptr;
}

/// A minimal one-UI manifest; `irMajor` and `version` are the parts under test.
std::string minimalManifest(int version, int irMajor) {
    return "{\"version\": " + std::to_string(version) +
           ", \"bundle\": {\"id\": \"mygame\", \"name\": \"MyGame UI\", \"irMajor\": " +
           std::to_string(irMajor) + "}, \"uis\": []}";
}

}  // namespace

TEST_CASE("bundle: opens the published fixture") {
    std::string error = "unchanged";
    Result<std::unique_ptr<Bundle>> result = Bundle::open(fixture("published"), nullptr, &error);
    REQUIRE(result.ok());
    CHECK(error.empty());
    Bundle& bundle = *result.value();

    CHECK(bundle.id() == "mygame");
    CHECK(bundle.name() == "MyGame UI");
    CHECK(bundle.dir() == fixture("published"));
    REQUIRE(bundle.uis().size() == 2);
    CHECK(bundle.uis()[0].id == "hud");
    CHECK(bundle.uis()[1].id == "pause");
    CHECK(bundle.ui("nope") == nullptr);

    const UiInfo* hud = bundle.ui("hud");
    REQUIRE(hud != nullptr);
    CHECK(hud->name == "HUD");
    CHECK(hud->file == "ui/hud.xtir");
    CHECK(hud->flow == "ui/hud.xtflow");
    CHECK(hud->thumb == "thumbs/hud.png");
    CHECK(hud->size.x == doctest::Approx(1280.0f));
    CHECK(hud->size.y == doctest::Approx(720.0f));
    REQUIRE(hud->tags.size() == 1);
    CHECK(hud->tags[0] == "ingame");
    CHECK(hud->requiresEffects);
    CHECK(hud->requiresSpace3d);
    CHECK(hud->requiresText);
    CHECK(hud->assets.size() == 2);
    CHECK(hud->fileHash == 1111u);

    const PortInfo* fire = findPort(*hud, "#fire.Click");
    REQUIRE(fire != nullptr);
    CHECK(fire->type == "pulse");
    CHECK_FALSE(fire->isInput);
    CHECK(fire->params.empty());

    const PortInfo* ammo = findPort(*hud, "#ammo.setProperty");
    REQUIRE(ammo != nullptr);
    CHECK(ammo->type == "number");
    CHECK(ammo->isInput);
    REQUIRE(ammo->params.count("prop") == 1);
    CHECK(ammo->params.at("prop") == "RotationY");

    const UiInfo* pause = bundle.ui("pause");
    REQUIRE(pause != nullptr);
    CHECK_FALSE(pause->requiresSpace3d);
    CHECK(pause->flow.empty());
    CHECK(pause->ports.empty());
}

TEST_CASE("bundle: rejects missing and incompatible manifests") {
    SUBCASE("missing directory") {
        std::string error;
        Result<std::unique_ptr<Bundle>> result = Bundle::open(fixture("published/nope"), nullptr, &error);
        CHECK(result.status() == Status::kNotFound);
        CHECK_FALSE(error.empty());
    }

    const std::string dir = copiedFixture("manifest_versions");

    SUBCASE("manifest version 2") {
        writeManifest(dir, minimalManifest(2, 2));
        std::string error;
        Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, &error);
        CHECK(result.status() == Status::kUnsupported);
        CHECK_FALSE(error.empty());
    }
    SUBCASE("irMajor mismatch") {
        writeManifest(dir, minimalManifest(1, 99));
        std::string error;
        Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, &error);
        CHECK(result.status() == Status::kUnsupported);
        CHECK(error.find("99") != std::string::npos);
        CHECK(error.find("2") != std::string::npos);
    }
    SUBCASE("not JSON") {
        writeManifest(dir, "}{ not json");
        std::string error;
        Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, &error);
        CHECK(result.status() == Status::kIoError);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("bundle: reload() reports changed and new UIs") {
    const std::string dir = copiedFixture("reload");
    Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, nullptr);
    REQUIRE(result.ok());
    Bundle& bundle = *result.value();
    REQUIRE(bundle.uis().size() == 2);

    // Nothing changed yet.
    CHECK(bundle.reload().empty());

    writeManifest(dir, R"({
      "version": 1,
      "bundle": { "id": "mygame", "name": "MyGame UI", "irMajor": 2 },
      "uis": [
        { "id": "hud",   "name": "HUD",   "file": "ui/hud.xtir",   "fileHash": 9999 },
        { "id": "pause", "name": "Pause", "file": "ui/pause.xtir", "fileHash": 2222 },
        { "id": "new",   "name": "New",   "file": "ui/new.xtir",   "fileHash": 3333 }
      ]
    })");
    const std::vector<std::string> changed = bundle.reload();
    REQUIRE(changed.size() == 2);
    CHECK(changed[0] == "hud");
    CHECK(changed[1] == "new");
    CHECK(bundle.uis().size() == 3);
    REQUIRE(bundle.ui("hud") != nullptr);
    CHECK(bundle.ui("hud")->fileHash == 9999u);

    // A broken manifest leaves the previous state alone.
    writeManifest(dir, "not json");
    CHECK(bundle.reload().empty());
    CHECK(bundle.uis().size() == 3);
    CHECK(bundle.id() == "mygame");
}

TEST_CASE("bundle: reload() re-registers the nine-slice and atlas meta and drops the old names (T-262a)") {
    const std::string dir = copiedFixture("reload_meta");
    writeManifest(dir, metaManifest("assets/1111111111111111.png", 0));
    Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, nullptr);
    REQUIRE(result.ok());
    Bundle& bundle = *result.value();
    REQUIRE(bundle.assets().imageMeta("assets/1111111111111111.png") != nullptr);

    // A re-pack gave the nine-slice image a new content name and moved the atlas cell.
    writeManifest(dir, metaManifest("assets/2222222222222222.png", 16));
    bundle.reload();
    REQUIRE(bundle.assetTable().size() == 2);
    CHECK(bundle.assetTable()[0].file == "assets/2222222222222222.png");
    REQUIRE(bundle.atlases().size() == 1);
    REQUIRE(bundle.atlases()[0].entries.size() == 1);
    CHECK(bundle.atlases()[0].entries[0].x == 16);
    const AssetResolver::ImageMeta* nine = bundle.assets().imageMeta("assets/2222222222222222.png");
    REQUIRE(nine != nullptr);
    CHECK(nine->hasSlice);
    CHECK(nine->slice[0] == 8.0f);
    CHECK(nine->slice[3] == 4.0f);
    CHECK(bundle.assets().imageMeta("assets/1111111111111111.png") == nullptr);
    const AssetResolver::ImageMeta* cell = bundle.assets().imageMeta("atlas:0#assets/cell.png");
    REQUIRE(cell != nullptr);
    CHECK(cell->inAtlas);
    CHECK(cell->x == 16);
    CHECK(cell->subUv.x == 0.25f);
}

TEST_CASE("bundle: assets() reads the deduplicated asset files") {
    Result<std::unique_ptr<Bundle>> result = Bundle::open(fixture("published"), nullptr, nullptr);
    REQUIRE(result.ok());
    Bundle& bundle = *result.value();

    const UiInfo* hud = bundle.ui("hud");
    REQUIRE(hud != nullptr);
    REQUIRE(hud->assets.size() == 2);

    std::vector<uint8_t> bytes;
    REQUIRE(bundle.assets().readBytes(hud->assets[0], bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "PNG00");

    bytes.clear();
    REQUIRE(bundle.assets().readBytes(hud->assets[1], bytes));
    CHECK(std::string(bytes.begin(), bytes.end()) == "TTF00");

    CHECK_FALSE(bundle.assets().readBytes("assets/missing.png", bytes));
}

TEST_CASE("bundle: a plain file that is not a bundle is not found") {
    // T-304: no container is recognised; a file goes the readManifest failure path. A 4-byte tag
    // and 28 zero bytes stand in for any content.
    const std::filesystem::path file = xt::test::tempDir() / "xt_bundle_t304_plain.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out.write("ABCD", 4);
        const std::string zeros(28, '\0');
        out.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    const std::string filePath = file.generic_string();

    std::string error;
    Result<std::unique_ptr<Bundle>> result = Bundle::open(filePath, nullptr, &error);
    CHECK_FALSE(result.ok());
    CHECK(result.status() == Status::kNotFound);
    CHECK_FALSE(error.empty());
    CHECK(error.find("plugin") == std::string::npos);

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("bundle: uis[].fit is read, and a missing fit is native (T-156)") {
    Result<std::unique_ptr<Bundle>> published = Bundle::open(fixture("published"), nullptr, nullptr);
    REQUIRE(published.ok());
    for (const UiInfo& ui : published.value()->uis()) {
        CHECK(ui.fit == "native");
    }

    const std::string dir = copiedFixture("fit");
    writeManifest(
        dir,
        "{\"version\": 1, \"bundle\": {\"id\": \"mygame\", \"name\": \"MyGame UI\", \"irMajor\": " +
            std::to_string(kIrFormatMajor) +
            "}, \"uis\": [{\"id\": \"hud\", \"fit\": \"contain\"}, {\"id\": \"menu\", \"fit\": \"cover\"},"
            " {\"id\": \"map\"}]}");
    std::string error;
    Result<std::unique_ptr<Bundle>> result = Bundle::open(dir, nullptr, &error);
    REQUIRE_MESSAGE(result.ok(), error);
    REQUIRE(result.value()->ui("hud") != nullptr);
    CHECK(result.value()->ui("hud")->fit == "contain");
    CHECK(result.value()->ui("menu")->fit == "cover");
    CHECK(result.value()->ui("map")->fit == "native");
}

namespace {

int g_readerCalls = 0;

/// T-240: an in-memory "APK" holding one manifest at `apk/bundle/manifest.json`.
bool memoryReader(const std::string& path, std::string& out, void* user) {
    ++g_readerCalls;
    CHECK(user == &g_readerCalls);
    if (path != "apk/bundle/manifest.json") {
        return false;
    }
    out = minimalManifest(1, static_cast<int>(kIrFormatMajor));
    return true;
}

}  // namespace

TEST_CASE("bundle: setBundleFileReader routes the manifest read (T-240)") {
    g_readerCalls = 0;
    setBundleFileReader(&memoryReader, &g_readerCalls);
    Result<std::unique_ptr<Bundle>> fromMemory = Bundle::open("apk/bundle", nullptr, nullptr);
    Result<std::unique_ptr<Bundle>> missing = Bundle::open("apk/other", nullptr, nullptr);
    Result<std::unique_ptr<Bundle>> realFixture = Bundle::open(fixture("published"), nullptr, nullptr);
    setBundleFileReader(nullptr);
    REQUIRE(fromMemory.ok());
    CHECK(fromMemory.value()->id() == "mygame");
    CHECK(missing.status() == Status::kNotFound);
    CHECK_FALSE(realFixture.ok());  // The reader, not the file system, answered.
    CHECK(g_readerCalls == 3);
    CHECK(Bundle::open(fixture("published"), nullptr, nullptr).ok());  // Restored.
}

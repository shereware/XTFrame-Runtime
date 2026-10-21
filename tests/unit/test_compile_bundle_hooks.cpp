#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node_arena.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// `xt_compile`'s bundle hooks: `CompileOptions::impliedStylesheets` (a bundle's `shared.styles`,
/// loaded as if it preceded the document's own `<link>`/`<style>`), `assetNameRewrite` (external
/// asset references rewritten to their in-bundle name) and `CompileResult::assets` (the ASST entry
/// list a bundle writer packs from). See docs/design/22-bundle.md §22.2-§22.3, docs/tasks/T-91.md.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

/// Loads `result`'s IR into a Document with a 400x300 viewport and one update(0), same as the
/// layout tests' fixture loader.
std::unique_ptr<Document> loadDoc(const CompileResult& result, TextShaper& shaper, AssetResolver& assets) {
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

float widthOfA(Document& doc) {
    ElementRef a = doc.byId("a");
    REQUIRE(a.valid());
    return doc.arena().element(a.node).box.width;
}

}  // namespace

TEST_CASE("compile: impliedStylesheets are loaded before the document's own styles") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);

    CompileOptions options;
    options.impliedStylesheets.push_back(layoutPath("bundle_hooks_shared.xts"));
    CompileResult result = compileDocument(layoutPath("bundle_hooks.xt"), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc = loadDoc(result, shaper, assets);
    CHECK(widthOfA(*doc) == doctest::Approx(40));

    // The document's own <style> is parsed after the implied sheet, so it wins at equal
    // specificity. Checked through compileDocumentFromStrings, which applies the same options.
    std::string mainText =
        "<xt><head><style>#a { width: 50px; height: 20px; }</style></head>"
        "<body><div id=\"a\"></div></body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"bundle_hooks_mem.xt", mainText}};
    CompileResult memResult = compileDocumentFromStrings(files, "bundle_hooks_mem.xt", options);
    REQUIRE(memResult.ok());
    std::unique_ptr<Document> memDoc = loadDoc(memResult, shaper, assets);
    CHECK(widthOfA(*memDoc) == doctest::Approx(50));
}

TEST_CASE("compile: an unreadable implied stylesheet is an error naming that path") {
    CompileOptions options;
    std::string missing = layoutPath("bundle_hooks_missing.xts");
    options.impliedStylesheets.push_back(missing);

    CompileResult result = compileDocument(layoutPath("bundle_hooks.xt"), options);
    CHECK(!result.ok());
    bool found = false;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.file == missing) {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("compile: assetNameRewrite renames external assets and fills CompileResult::assets") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);

    CompileOptions options;
    options.rootDir = XT_TEST_LAYOUT_DIR;
    options.assetNameRewrite = [](std::string_view, std::string_view, uint8_t kind) {
        return "assets/" + std::to_string(static_cast<int>(kind)) + ".bin";
    };

    // The font file does not exist on disk: that is a warning, not an error, and the rewrite still
    // applies (the bundle writer decides what to do with an unresolved reference).
    std::string mainText =
        "<xt><head><style>@font-face { font-family: \"bundled\"; src: \"x.ttf\"; }</style></head>"
        "<body><img src=\"bundle_hooks_img.png\"/></body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"bundle_hooks_mem.xt", mainText}};
    CompileResult result = compileDocumentFromStrings(files, "bundle_hooks_mem.xt", options);
    REQUIRE(result.ok());

    // @font-face assets are emitted in step 5, `<img src>` assets in step 6, so the font comes
    // first in AssetId order.
    REQUIRE(result.assets.size() == 2);
    const CompiledAsset& font = result.assets[0];
    CHECK(font.kind == 1);
    CHECK(font.sourceName == "x.ttf");
    CHECK(font.name == "assets/1.bin");
    CHECK(font.resolvedPath.empty());
    CHECK(!font.embedded);
    const CompiledAsset& image = result.assets[1];
    CHECK(image.kind == 0);
    CHECK(image.sourceName == "bundle_hooks_img.png");
    CHECK(image.name == "assets/0.bin");
    CHECK(!image.resolvedPath.empty());
    CHECK(!image.embedded);

    // The rewritten names are what actually landed in ASST.
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    REQUIRE(view.value().assets().assets().size() == 2);
    CHECK(view.value().string(view.value().assets().assets()[0].name) == "assets/1.bin");
    CHECK(view.value().string(view.value().assets().assets()[1].name) == "assets/0.bin");
}

TEST_CASE("compile: an embedded asset keeps its source name and skips assetNameRewrite") {
    CompileOptions options;
    options.rootDir = XT_TEST_LAYOUT_DIR;
    options.embedImages = true;
    int rewriteCalls = 0;
    options.assetNameRewrite = [&rewriteCalls](std::string_view, std::string_view, uint8_t) {
        ++rewriteCalls;
        return std::string("assets/rewritten.bin");
    };

    std::string mainText = "<xt><body><img src=\"bundle_hooks_img.png\"/></body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"bundle_hooks_mem.xt", mainText}};
    CompileResult result = compileDocumentFromStrings(files, "bundle_hooks_mem.xt", options);
    REQUIRE(result.ok());

    CHECK(rewriteCalls == 0);
    REQUIRE(result.assets.size() == 1);
    CHECK(result.assets[0].embedded);
    CHECK(result.assets[0].name == "bundle_hooks_img.png");
    CHECK(result.assets[0].sourceName == "bundle_hooks_img.png");
}

TEST_CASE("compile: isMachinePath reads Windows and UNC paths on every platform (T-273b)") {
    CHECK(isMachinePath("C:/art/a.png"));
    CHECK(isMachinePath("c:\\art\\a.png"));
    CHECK(isMachinePath("C:a.png"));
    CHECK(isMachinePath("//server/share/a.png"));
    CHECK(isMachinePath("\\\\server\\share\\a.png"));
    CHECK(isMachinePath("/art/a.png"));
    CHECK_FALSE(isMachinePath("img/a.png"));
    CHECK_FALSE(isMachinePath("../a.png"));
    CHECK_FALSE(isMachinePath("engine://video"));
    CHECK_FALSE(isMachinePath(""));

    CHECK_FALSE(assetLeavesProject("img/a.png", "proj/ui", "proj"));
    CHECK_FALSE(assetLeavesProject("../shared/a.png", "proj/ui", "proj"));
    CHECK(assetLeavesProject("../../a.png", "proj/ui", "proj"));
    CHECK(assetLeavesProject("C:/art/a.png", "proj/ui", "proj"));
    CHECK_FALSE(assetLeavesProject("engine://video:intro", "proj/ui", "proj"));
    CHECK(assetLeavesProject("../a.png", "proj", ""));  // No root: the document's directory.
}

TEST_CASE("compile: pathInsideRoot / referencePath, the one definition of inside the project (T-273c)") {
    CHECK(pathInsideRoot(pathFromUtf8("/proj"), pathFromUtf8("/proj")));
    CHECK(pathInsideRoot(pathFromUtf8("/proj/ui/a.png"), pathFromUtf8("/proj/")));
    CHECK(pathInsideRoot(pathFromUtf8("/proj/ui/../shared/a.png"), pathFromUtf8("/proj")));
    CHECK_FALSE(pathInsideRoot(pathFromUtf8("/proj/../x/a.png"), pathFromUtf8("/proj")));
    CHECK_FALSE(pathInsideRoot(pathFromUtf8("/project/a.png"), pathFromUtf8("/proj")));  // Not a prefix.
    CHECK_FALSE(pathInsideRoot(pathFromUtf8("/art/a.png"), pathFromUtf8("/proj")));
    CHECK(pathInsideRoot(pathFromUtf8("proj/ui"), pathFromUtf8("proj")));
    CHECK_FALSE(pathInsideRoot(pathFromUtf8("../a.png"), pathFromUtf8(".")));
#ifdef _WIN32
    CHECK(pathInsideRoot(pathFromUtf8("c:/PROJ/Shared/a.png"), pathFromUtf8("C:/proj")));  // Case.
    CHECK_FALSE(pathInsideRoot(pathFromUtf8("D:/proj/a.png"), pathFromUtf8("C:/proj")));
#endif

    // A drive-letter or rooted reference is taken as written on every platform, never joined.
    CHECK(referencePath("C:/art/a.png", pathFromUtf8("/proj/ui")) == pathFromUtf8("C:/art/a.png"));
    CHECK(referencePath("/art/./a.png", pathFromUtf8("/proj/ui")) == pathFromUtf8("/art/a.png"));
    CHECK(referencePath("../img/a.png", pathFromUtf8("/proj/ui")) == pathFromUtf8("/proj/img/a.png"));
    CHECK_FALSE(
        pathInsideRoot(referencePath("C:/proj/a.png", pathFromUtf8("/proj/ui")), pathFromUtf8("/proj")));
}

TEST_CASE("compile: publishAssetNames embeds an outside image under its file name, -2 on a clash (T-273b)") {
    namespace fs = std::filesystem;
    const fs::path base = xt::test::tempDir() / "xt_compile_publish_names";
    std::error_code ec;
    fs::remove_all(base, ec);
    for (const char* dir : {"one", "two", "proj/ui"}) {
        fs::create_directories(base / dir, ec);
    }
    for (const char* dir : {"one", "two"}) {
        fs::copy_file(layoutPath("bundle_hooks_img.png"), base / dir / "pic.png", ec);
        REQUIRE(ec.value() == 0);
    }
    const std::string one = (base / "one").generic_string();
    const std::string two = (base / "two").generic_string();
    const std::string mainText = "<xt><body><img id=\"a\" src=\"" + one + "/pic.png\"/><img id=\"b\" src=\"" +
                                 two + "/pic.png\"/><img id=\"c\" src=\"../../one/pic.png\"/></body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"publish.xt", mainText}};

    CompileOptions options;
    options.rootDir = (base / "proj").generic_string();
    options.xtDir = (base / "proj" / "ui").generic_string();
    options.embedImages = true;

    // Not publishing (editor preview, xtframe-compile): every name as written.
    CompileResult plain = compileDocumentFromStrings(files, "publish.xt", options);
    REQUIRE(plain.ok());
    REQUIRE(plain.assets.size() == 3);
    CHECK(plain.assets[0].name == one + "/pic.png");
    CHECK(plain.assets[2].name == "../../one/pic.png");

    options.publishAssetNames = true;
    CompileResult published = compileDocumentFromStrings(files, "publish.xt", options);
    REQUIRE(published.ok());
    REQUIRE(published.assets.size() == 3);
    CHECK(published.assets[0].name == "pic.png");
    CHECK(published.assets[1].name == "pic-2.png");
    CHECK(published.assets[2].name == "pic-3.png");
    CHECK(published.assets[0].sourceName == one + "/pic.png");
    Result<IrView> view = IrView::open(IrBlob{published.ir.data(), published.ir.size()}, IrReadOptions{true});
    REQUIRE(view.ok());
    const IrView& ir = view.value();
    for (uint32_t i = 0; i < ir.strings().count(); ++i) {
        CAPTURE(ir.string(i));
        CHECK(ir.string(i).find(base.generic_string()) == std::string_view::npos);
        CHECK(ir.string(i).find("../") == std::string_view::npos);
    }
    // Every <img src> is the very string-table entry of its ASST name.
    size_t img = 0;
    for (const IrDocElement& el : ir.doc().elements()) {
        if (ir.string(el.tag) != "img") {
            continue;
        }
        REQUIRE(img < 3);
        bool found = false;
        for (uint32_t a = 0; a < el.attrCount; ++a) {
            const IrAttr& attr = ir.doc().attrPool()[el.attrOffset + a];
            if (ir.string(attr.name) == "src") {
                CHECK(attr.value == ir.assets().assets()[img].name);
                found = true;
            }
        }
        CHECK(found);
        ++img;
    }
    CHECK(img == 3);
}

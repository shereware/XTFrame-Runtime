// T-808 items 1-3 (Docs/tasks/T-808.md; Docs/design/23-host-integration.md §23.8b, §23.ah, §23.aj): an
// external (`engine://`) texture named by a CSS `url()`.
//   1. An unquoted `url(engine://a)` is CSS's url token: the `//` (or a `/*`) inside it is part of the
//      URL, and it compiles exactly as the quoted `url("engine://a")`. Comments elsewhere still go.
//   2. A `style` attribute's `url(engine://a)` is the same declaration as in a `<style>` rule.
//   3. A `background-image` / `border-image-source` naming an external texture is sized by the binding
//      (FileAssetResolver::imageSize), 0 x 0 while unbound, and follows a new size.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"

using namespace xt;
using namespace xt::compile;

namespace {

std::string pageT808(std::string_view style, std::string_view body) {
    return "<xt><head><style>body { margin: 0; padding: 0; } " + std::string(style) +
           "</style></head>\n<body>" + std::string(body) + "</body></xt>";
}

CompileResult compileT808(const std::string& text) {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_XT_DIR;
    const std::vector<SourceFile> files{{"main.xt", text}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

/// The compiled IR of `text`; no Error, no Warning.
std::vector<uint8_t> irT808(const std::string& text) {
    const CompileResult result = compileT808(text);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Warning);
    }
    test::requireCompiled(result);
    return result.ir;
}

/// True when `result` has the external asset `name` (written with its scheme).
bool hasExternalT808(const CompileResult& result, std::string_view name) {
    for (const CompiledAsset& a : result.assets) {
        if (a.name == name && a.external) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kBoxes =
    "#a, #b { position: absolute; left: 0; top: 0; width: 100px; height: 60px; border-width: 4px; }";

struct LoadedT808 {
    std::vector<uint8_t> ir;
    std::unique_ptr<Document> doc;

    const ElementData& element(std::string_view id) { return doc->arena().element(doc->byId(id).node); }
};

/// `text` loaded against `assets` and updated once.
LoadedT808 loadT808(const std::string& text, AssetResolver& assets) {
    LoadedT808 out;
    out.ir = irT808(text);
    REQUIRE(Document::load(IrBlob{out.ir.data(), out.ir.size()}, false, nullptr, &assets, out.doc) ==
            Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    out.doc->update(0);
    return out;
}

}  // namespace

TEST_CASE("T-808 url(): an unquoted engine:// url compiles exactly as the quoted one") {
    const std::string quoted = pageT808(
        std::string(kBoxes) + R"(#a { background-image: url("engine://a"); background-repeat: no-repeat; }
#b { border-image-source: url("engine://a"); })",
        R"(<div id="a"></div><div id="b"></div>)");
    const std::string unquoted = pageT808(
        std::string(kBoxes) + R"(#a { background-image: url(engine://a); background-repeat: no-repeat; }
#b { border-image-source: url( engine://a ); })",
        R"(<div id="a"></div><div id="b"></div>)");
    const CompileResult result = compileT808(unquoted);
    test::requireCompiled(result);
    CHECK(hasExternalT808(result, "engine://a"));
    CHECK(irT808(unquoted) == irT808(quoted));

    // The border-image shorthand and an upper-case URL( too.
    CHECK(irT808(pageT808("#b { border-image: URL(engine://a) 10 fill; }", R"(<div id="b"></div>)")) ==
          irT808(pageT808(R"(#b { border-image: url("engine://a") 10 fill; })", R"(<div id="b"></div>)")));
}

TEST_CASE("T-808 url(): comments around an unquoted url still go, a /* inside it is part of the URL") {
    const std::string body = R"(<div id="a"></div>)";
    CHECK(irT808(pageT808("/* a */ #a { /* b */ background-image: /* c */ url(engine://a) /* d */; }",
                          body)) == irT808(pageT808(R"(#a { background-image: url("engine://a"); })", body)));
    // CSS Syntax 3: an unquoted url runs to its `)`, so `/*` there is not a comment.
    CHECK(irT808(pageT808("#a { background-image: url(engine://a/*b); }", body)) ==
          irT808(pageT808(R"(#a { background-image: url("engine://a/*b"); })", body)));
    // A path with `/` unquoted (rejected before: '/' was no token at all).
    CHECK(irT808(pageT808("#a { background-image: url(engine://dir/pic.png); }", body)) ==
          irT808(pageT808(R"(#a { background-image: url("engine://dir/pic.png"); })", body)));
}

TEST_CASE("T-808 url(): a style attribute names an engine:// texture like a <style> rule") {
    for (const char* value : {"url(engine://a)", "url('engine://a')"}) {
        CAPTURE(value);
        const std::string text = pageT808(
            "", std::string(R"(<div id="a" style="width: 10px; height: 10px; background-image: )") + value +
                    R"("></div><div id="b" style="width: 10px; height: 10px; border-image: )" + value +
                    R"( 2"></div>)");
        const CompileResult result = compileT808(text);
        for (const Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != Diagnostic::Level::Warning);
        }
        test::requireCompiled(result);
        CHECK(hasExternalT808(result, "engine://a"));

        FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
        assets.setExternalTexture("a", 5, 40, 20);
        LoadedT808 l = loadT808(text, assets);
        REQUIRE(l.element("a").style.backgroundImage != kInvalidAsset);
        CHECK(isExternalAsset(l.element("a").style.backgroundImage, l.doc->ir()));
        REQUIRE(l.element("b").style.borderImage != kInvalidAsset);
        CHECK(isExternalAsset(l.element("b").style.borderImage, l.doc->ir()));
    }
}

TEST_CASE("T-808 imageSize: a background / border image naming an external texture has the binding's size") {
    const std::string text = pageT808(
        std::string(kBoxes) + R"(#a { background-image: url("engine://a"); background-repeat: no-repeat; }
#b { border-image: url("engine://a") 50% fill; })",
        R"(<div id="a"></div><div id="b"></div>)");
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};

    SUBCASE("unbound: 0 x 0, as before") {
        LoadedT808 l = loadT808(text, assets);
        CHECK(l.element("a").backgroundImageSize.x == 0.0f);
        CHECK(l.element("b").borderImageSize.x == 0.0f);
    }
    SUBCASE("bound before the load, then resized") {
        assets.setExternalTexture("a", 5, 40, 20);
        LoadedT808 l = loadT808(text, assets);
        CHECK(l.element("a").backgroundImageSize.x == doctest::Approx(40.0f));
        CHECK(l.element("a").backgroundImageSize.y == doctest::Approx(20.0f));
        CHECK(l.element("b").borderImageSize.x == doctest::Approx(40.0f));
        CHECK(l.element("b").borderImageSize.y == doctest::Approx(20.0f));
        CHECK(l.element("b").borderImageSlicePx[0] == doctest::Approx(20.0f));  // 50% of 40
        CHECK(l.element("b").borderImageSlicePx[2] == doctest::Approx(10.0f));  // 50% of 20

        // §23.ah item 1: a new size reaches the users at the next update.
        assets.setExternalTexture("a", 6, 80, 10);
        l.doc->update(1.0f / 60.0f);
        CHECK(l.element("a").backgroundImageSize.x == doctest::Approx(80.0f));
        CHECK(l.element("a").backgroundImageSize.y == doctest::Approx(10.0f));
        CHECK(l.element("b").borderImageSize.x == doctest::Approx(80.0f));
        CHECK(l.element("b").borderImageSlicePx[0] == doctest::Approx(40.0f));

        // Unbound again: 0 x 0.
        assets.clearExternalTexture("a");
        l.doc->update(1.0f / 60.0f);
        CHECK(l.element("a").backgroundImageSize.x == 0.0f);
        CHECK(l.element("b").borderImageSize.x == 0.0f);
    }
    SUBCASE("a planar binding (§23.aj) is sized by its picture, not by its R8 texture") {
        assets.setExternalTexture("a", 5, 64, 32);
        AssetResolver::ExternalPlanes planes;
        planes.layout = AssetResolver::ExternalLayout::PackedYuv420;
        planes.textureWidth = 64;
        planes.textureHeight = 48;
        planes.y = {0, 0, 64, 32};
        planes.u = {0, 32, 32, 16};
        planes.v = {32, 32, 32, 16};
        assets.setExternalTexturePlanes("a", planes);
        LoadedT808 l = loadT808(text, assets);
        CHECK(l.element("a").backgroundImageSize.x == doctest::Approx(64.0f));
        CHECK(l.element("a").backgroundImageSize.y == doctest::Approx(32.0f));
        CHECK(l.element("b").borderImageSize.y == doctest::Approx(32.0f));
    }
}

// T-828 (Docs/tasks/T-828.md): the T-806 report re-checked on the current compiler; the rest of the
// sheet's test list that T-808 did not cover.
namespace {

/// The messages of `result`'s warnings, in order.
std::vector<std::string> warningsT828(const CompileResult& result) {
    std::vector<std::string> out;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning) {
            out.push_back(d.message);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("T-828 url(): stylesheet and style attribute, quoted and unquoted, give the same element") {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    assets.setExternalTexture("a", 5, 40, 20);
    const char* const kBox = "width: 100px; height: 60px; border-width: 4px; ";
    for (const char* url : {"url(engine://a)", "url('engine://a')"}) {
        for (const bool inlineStyle : {false, true}) {
            CAPTURE(url);
            CAPTURE(inlineStyle);
            const std::string decls = std::string(kBox) + "background-image: " + url +
                                      "; background-repeat: no-repeat; border-image-source: " + url + ";";
            const std::string text = inlineStyle
                                         ? pageT808("", R"(<div id="a" style=")" + decls + R"("></div>)")
                                         : pageT808("#a { " + decls + " }", R"(<div id="a"></div>)");
            LoadedT808 l = loadT808(text, assets);
            const ElementData& e = l.element("a");
            REQUIRE(e.style.backgroundImage != kInvalidAsset);
            CHECK(isExternalAsset(e.style.backgroundImage, l.doc->ir()));
            REQUIRE(e.style.borderImage != kInvalidAsset);
            CHECK(isExternalAsset(e.style.borderImage, l.doc->ir()));
            CHECK(e.backgroundImageSize.x == doctest::Approx(40.0f));
            CHECK(e.borderImageSize.y == doctest::Approx(20.0f));
        }
    }
}

TEST_CASE("T-828 url(): an unquoted url with // that is not engine:// is reported by its address") {
    const std::string body = R"(<div id="a"></div>)";
    const CompileResult unquoted =
        compileT808(pageT808("#a { background-image: url(http://example.invalid/pic.png); }", body));
    const CompileResult quoted =
        compileT808(pageT808(R"(#a { background-image: url("http://example.invalid/pic.png"); })", body));
    test::requireCompiled(unquoted);
    const std::vector<std::string> w = warningsT828(unquoted);
    REQUIRE(w.size() == 1);
    CHECK(w[0].find("\"http://example.invalid/pic.png\"") != std::string::npos);
    CHECK(w[0].find("not found") != std::string::npos);
    CHECK(w == warningsT828(quoted));
    CHECK(unquoted.ir == quoted.ir);

    // The same in a style attribute: the declaration is kept (not dropped as malformed).
    const CompileResult inl = compileT808(pageT808(
        "", R"x(<div id="a" style="background-image: url(http://example.invalid/pic.png)"></div>)x"));
    test::requireCompiled(inl);
    const std::vector<std::string> wi = warningsT828(inl);
    REQUIRE(wi.size() == 1);
    CHECK(wi[0].find("\"http://example.invalid/pic.png\"") != std::string::npos);
}

TEST_CASE("T-828 CSS comments: /* */ still goes; // is not a comment outside url()") {
    const std::string body = R"(<div id="a"></div>)";
    CHECK(irT808(pageT808("#a { /* x // y */ width: 10px; }", body)) ==
          irT808(pageT808("#a { width: 10px; }", body)));
    // `//` is not CSS comment syntax: the declaration it starts is reported, not skipped silently.
    const CompileResult slashes = compileT808(pageT808("#a { // width: 10px;\n height: 5px; }", body));
    CHECK_FALSE(slashes.diagnostics.empty());
}

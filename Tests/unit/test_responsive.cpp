#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-345 (Docs/design/25-editor-blueprint.md §25.4.2, 05-style-and-css.md §5.9.9): the project
/// scale policy (Context::setUiScaleLimits), `ui-scale` (auto / none / clamp) on lengths, font
/// sizes and inheritance, `media-override` against `@media`, and the `@3x` variant choice.

using namespace xt;

namespace {

namespace fs = std::filesystem;

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"responsive.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "responsive.xt", compile::CompileOptions{});
}

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body) {
    compile::CompileResult result = compileRaw(css, body);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

/// One Context + one loaded Document laid out in a `w` x `h` viewport.
struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;

    Loaded(const std::vector<uint8_t>& ir, float w, float h) {
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, w, h}, 1.0f);
        ctx.update(0.0f);
    }

    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    const ComputedStyle& style(const char* id) { return doc->arena().element(node(id)).style; }
    const LayoutBox& box(const char* id) { return doc->arena().element(node(id)).box; }
    float width(const char* id) { return box(id).width; }
    float height(const char* id) { return box(id).height; }
    float fontSize(const char* id) { return style(id).base[prop::FontSize].len.value; }
    bool shown(const char* id) { return style(id).display != Display::None; }
    void uiScale(float s) {
        REQUIRE(ctx.setUiScale(s) == Status::kOk);
        ctx.update(0.0f);
    }
    void dpr(float d) {
        DeviceInfo device = ctx.device();
        device.dpr = d;
        REQUIRE(ctx.setDevice(device) == Status::kOk);
        ctx.update(0.0f);
    }
};

const char* kAbs = "position:absolute;left:0;top:0;";

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

/// Fake decoder: a 1-pixel-high image whose width is the byte count.
bool fakeDecode(const uint8_t* bytes, size_t size, ImageData& out) {
    if (size == 0) {
        return false;
    }
    out.width = static_cast<int>(size);
    out.height = 1;
    out.rgba.assign(size * 4, 255);
    (void)bytes;
    return true;
}

/// A resolver whose every image is 40 x 20.
class FixedSizeResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 40;
        out.height = 20;
        out.rgba.assign(40 * 20 * 4, 255);
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{40, 20}; }
};

}  // namespace

// -------------------------------------------------------------------------------------------
// The project factor: fit (bundle::uiScaleFor, Tests/bundle), clamp and the DPI floor.
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: Context::setUiScaleLimits clamps the factor, then raises it to dpiFloor x dpr") {
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    REQUIRE(ctx.setUiScale(2.0f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(2.0f));
    REQUIRE(ctx.setUiScaleLimits(0.75f, 1.5f, 0.0f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(1.5f));      // 4K does not blow up past 1.5.
    CHECK(ctx.uiScaleBase() == doctest::Approx(2.0f));  // The rule's factor is kept.
    REQUIRE(ctx.setUiScale(0.5f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(0.75f));  // 720p is not shrunk below 0.75.
    REQUIRE(ctx.setUiScale(1.0f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(1.0f));

    // The DPI floor follows setDevice()'s dpr.
    REQUIRE(ctx.setUiScaleLimits(0.0f, std::numeric_limits<float>::infinity(), 0.6f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(1.0f));  // 1 >= 0.6 x 1.
    DeviceInfo device;
    device.dpr = 2.0f;
    REQUIRE(ctx.setDevice(device) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(1.2f));  // max(1, 0.6 x 2).
    device.dpr = 1.0f;
    REQUIRE(ctx.setDevice(device) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(1.0f));
    // Clamp first, then the floor.
    REQUIRE(ctx.setUiScaleLimits(0.25f, 0.5f, 0.6f) == Status::kOk);
    CHECK(ctx.uiScale() == doctest::Approx(0.6f));

    // Invalid limits change nothing.
    CHECK(ctx.setUiScaleLimits(-1.0f, 2.0f, 0.0f) == Status::kInvalidArgument);
    CHECK(ctx.setUiScaleLimits(2.0f, 1.0f, 0.0f) == Status::kInvalidArgument);
    CHECK(ctx.setUiScaleLimits(0.0f, std::nanf(""), 0.0f) == Status::kInvalidArgument);
    CHECK(ctx.setUiScaleLimits(0.0f, 2.0f, -0.5f) == Status::kInvalidArgument);
    CHECK(ctx.uiScale() == doctest::Approx(0.6f));
}

// -------------------------------------------------------------------------------------------
// ui-scale
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: ui-scale auto / none / clamp() resolve px, em, font-size and border with their factor") {
    const std::string box = std::string(kAbs) + "width:10px;height:4em;font-size:5px;border:1px red;";
    const std::string css =
        "#a{" + box + "} #n{" + box + "ui-scale:none} #c{" + box + "ui-scale:clamp(1, 1.5)}";
    Loaded l(compileDoc(css, R"(<div id="a"></div><div id="n"></div><div id="c"></div>)"), 400, 200);
    // uiscale 1: every factor is 1 (border-box widths).
    CHECK(l.width("a") == doctest::Approx(10));
    CHECK(l.width("n") == doctest::Approx(10));
    CHECK(l.width("c") == doctest::Approx(10));
    CHECK(l.style("a").uiScaleMode == kUiScaleAuto);
    CHECK(l.style("n").uiScaleMode == kUiScaleNone);
    CHECK(l.style("c").uiScaleMode == kUiScaleClamp);
    CHECK(l.style("c").uiScaleMin == doctest::Approx(1.0f));
    CHECK(l.style("c").uiScaleMax == doctest::Approx(1.5f));

    l.uiScale(2.0f);
    CHECK(l.style("a").uiScaleEff == doctest::Approx(2.0f));
    CHECK(l.style("n").uiScaleEff == doctest::Approx(1.0f));
    CHECK(l.style("c").uiScaleEff == doctest::Approx(1.5f));
    CHECK(l.width("a") == doctest::Approx(20));
    CHECK(l.width("n") == doctest::Approx(10));  // Device px x dpr 1.
    CHECK(l.width("c") == doctest::Approx(15));
    CHECK(l.style("a").base[prop::BorderLeftWidth].len.value == doctest::Approx(2));
    CHECK(l.style("n").base[prop::BorderLeftWidth].len.value == doctest::Approx(1));
    CHECK(l.style("c").base[prop::BorderLeftWidth].len.value == doctest::Approx(1.5));
    CHECK(l.fontSize("a") == doctest::Approx(10));
    CHECK(l.fontSize("n") == doctest::Approx(5));
    CHECK(l.fontSize("c") == doctest::Approx(7.5));
    CHECK(l.height("a") == doctest::Approx(40));  // 4em of the scaled font.
    CHECK(l.height("n") == doctest::Approx(20));
    CHECK(l.height("c") == doctest::Approx(30));

    // none = 1 x dpr.
    l.dpr(2.0f);
    CHECK(l.style("n").uiScaleEff == doctest::Approx(2.0f));
    CHECK(l.width("n") == doctest::Approx(20));
}

TEST_CASE("T-345: % / vw / vh / uiscale units and calc() keep their meaning; calc's px terms scale") {
    const std::string css = std::string("#p{") + kAbs +
                            "width:200px;height:100px} "
                            "#a{position:absolute;width:50%;height:10vh} "
                            "#b{position:absolute;width:10uiscale;height:calc(10px + 10%)} "
                            "#n{position:absolute;ui-scale:none;width:50%;height:calc(10px + 10%)} "
                            "#u{position:absolute;ui-scale:none;width:10uiscale;height:5vw}";
    Loaded l(compileDoc(css, R"(<div id="p"><div id="a"></div><div id="b"></div><div id="n"></div>)"
                             R"(<div id="u"></div></div>)"),
             400, 200);
    l.uiScale(2.0f);
    CHECK(l.width("p") == doctest::Approx(400));
    CHECK(l.width("a") == doctest::Approx(200));  // 50 % of the (scaled) parent.
    CHECK(l.height("a") == doctest::Approx(20));  // vh: unchanged.
    CHECK(l.width("b") == doctest::Approx(20));   // 10uiscale = 10 x 2, not scaled again.
    CHECK(l.height("b") == doctest::Approx(40));  // calc(10px x 2 + 10 % of 200).
    CHECK(l.width("n") == doctest::Approx(200));  // % is relative to the parent either way.
    CHECK(l.height("n") == doctest::Approx(30));  // calc(10px x 1 + 20).
    CHECK(l.width("u") == doctest::Approx(20));   // `uiscale` units are the Document's uiscale.
    CHECK(l.height("u") == doctest::Approx(20));  // 5vw.
    l.uiScale(3.0f);
    CHECK(l.height("b") == doctest::Approx(30 + 30));  // The program follows a new uiscale.
    CHECK(l.height("n") == doctest::Approx(10 + 30));
}

TEST_CASE("T-345: ui-scale is inherited; a child can opt back in; inherited font sizes rescale") {
    const std::string css = std::string("#n{") + kAbs +
                            "ui-scale:none;font-size:10px} "
                            "#n div{width:10px;height:10px} "
                            "#n #back{ui-scale:auto} "
                            "#a{position:absolute;left:0;top:100px;font-size:10px} "
                            "#a div{ui-scale:none;width:1em;height:1em}";
    Loaded l(compileDoc(css, R"(<div id="n"><div id="kid"></div><div id="back"></div></div>)"
                             R"(<div id="a"><div id="fixed"></div></div>)"),
             400, 200);
    l.uiScale(2.0f);
    CHECK(l.style("kid").uiScaleMode == kUiScaleNone);  // Inherited.
    CHECK(l.width("kid") == doctest::Approx(10));
    CHECK(l.width("back") == doctest::Approx(20));     // auto again.
    CHECK(l.fontSize("back") == doctest::Approx(20));  // The parent's 10 design px, in its scale.
    CHECK(l.fontSize("a") == doctest::Approx(20));
    CHECK(l.fontSize("fixed") == doctest::Approx(10));  // Inherited 10 design px at factor 1.
    CHECK(l.width("fixed") == doctest::Approx(10));     // 1em of that.
}

TEST_CASE("T-345: setStyle(ui-scale) relayouts the subtree; a rescale starts no transition") {
    const std::string css = std::string("#a{") + kAbs +
                            "width:10px;height:10px;transition:width 1s linear} #a div{width:5px;height:5px}";
    Loaded l(compileDoc(css, R"(<div id="a"><div id="kid"></div></div>)"), 400, 200);
    l.uiScale(2.0f);
    CHECK(l.width("a") == doctest::Approx(20));  // Rescaled at once, not transitioned.
    CHECK(l.width("kid") == doctest::Approx(10));
    REQUIRE(l.doc->byId("a").setStyle("ui-scale: none") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == doctest::Approx(10));
    CHECK(l.width("kid") == doctest::Approx(5));  // The subtree follows.
    l.ctx.update(0.5f);
    CHECK(l.width("a") == doctest::Approx(10));
    REQUIRE(l.doc->byId("a").setStyle("ui-scale: clamp(1.25, 4)") == Status::kOk);
    l.ctx.update(0.6f);
    CHECK(l.width("kid") == doctest::Approx(10));  // clamp(2, 1.25, 4) = 2.
    CHECK(l.doc->byId("a").setStyle("ui-scale: clamp(2, 1)") == Status::kInvalidArgument);
}

TEST_CASE("T-345: a keyframe's px value is multiplied by the element's factor") {
    const std::string css = std::string("@keyframes grow { from { width: 10px } to { width: 30px } } #a{") +
                            kAbs + "height:10px;animation:grow 1s linear forwards} #n{" + kAbs +
                            "height:10px;ui-scale:none;animation:grow 1s linear forwards}";
    Loaded l(compileDoc(css, R"(<div id="a"></div><div id="n"></div>)"), 400, 200);
    l.uiScale(2.0f);
    l.ctx.update(0.5f);
    CHECK(l.width("a") == doctest::Approx(40));  // 20 design px x 2.
    CHECK(l.width("n") == doctest::Approx(20));
}

TEST_CASE("T-345: ui-scale parses auto, none and clamp() and rejects the rest") {
    for (const char* bad : {"ui-scale: clamp(0, 2)", "ui-scale: clamp(2, 1)", "ui-scale: half",
                            "ui-scale: clamp(1)", "media-override: always"}) {
        CAPTURE(bad);
        const compile::CompileResult r = compileRaw(std::string("#a{") + bad + "}", R"(<div id="a"></div>)");
        CHECK(std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [](const compile::Diagnostic& d) {
            return d.level != compile::Diagnostic::Level::Error;
        }));
    }
    Loaded l(compileDoc("#a{ui-scale:clamp(0.5, 2)} #b{ui-scale:none} #c{ui-scale:auto}",
                        R"(<div id="a"></div><div id="b"></div><div id="c"></div>)"),
             100, 100);
    CHECK(l.style("a").uiScaleMode == kUiScaleClamp);
    CHECK(l.style("b").uiScaleMode == kUiScaleNone);
    CHECK(l.style("c").uiScaleMode == kUiScaleAuto);
}

// -------------------------------------------------------------------------------------------
// media-override
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: media-override keeps @media rules from switching display; hide always hides") {
    const std::string css =
        "#auto, #show, #hide { width:10px; height:10px } "
        "#show { media-override: show } #hide { media-override: hide } "
        "#group { media-override: show } "
        "@media (max-width: 300px) { #auto, #show, #inner { display: none } #show { width: 20px } } "
        "@media (min-width: 301px) { #late { display: none } }";
    Loaded l(compileDoc(css, R"(<div id="auto"></div><div id="show"></div><div id="hide"></div>)"
                             R"(<div id="group"><div id="inner"></div><div id="late"></div></div>)"),
             400, 200);
    CHECK(l.shown("auto"));
    CHECK(l.shown("show"));
    CHECK_FALSE(l.shown("hide"));
    CHECK(l.shown("inner"));
    CHECK(l.shown("late"));  // Inherited `show`: the min-width rule does not hide it.
    CHECK(l.style("inner").mediaOverride == kMediaOverrideShow);

    l.ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    l.ctx.update(0.0f);
    CHECK_FALSE(l.shown("auto"));                   // The media rule applies as usual.
    CHECK(l.shown("show"));                         // Its display switch is skipped ...
    CHECK(l.width("show") == doctest::Approx(20));  // ... its other declarations are not.
    CHECK_FALSE(l.shown("hide"));
    CHECK(l.shown("inner"));

    // A runtime change.
    REQUIRE(l.doc->byId("auto").setStyle("media-override: show") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.shown("auto"));
    REQUIRE(l.doc->byId("auto").setStyle("media-override: hide") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK_FALSE(l.shown("auto"));
}

// -------------------------------------------------------------------------------------------
// Layout snapshot: a `none` subtree keeps its size under uiscale 2.
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: ui_scale_none -- the ui-scale: none subtree keeps its size under uiscale 2") {
    compile::CompileResult result =
        compile::compileDocument(layoutPath("ui_scale_none.xt"), compile::CompileOptions{});
    REQUIRE(result.ok());
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, &assets, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    LayoutEngine engine;
    engine.init(doc->ir(), &assets);
    const LayoutBox before = doc->arena().element(doc->byId("fixed").node).box;
    doc->setUiScale(2.0f);
    doc->update(0.0f);
    const std::string expected = readFile(layoutPath("ui_scale_none.json"));
    REQUIRE(!expected.empty());
    CHECK(engine.snapshot(doc->arena(), doc->root().node) == expected);
    const LayoutBox& after = doc->arena().element(doc->byId("fixed").node).box;
    CHECK(after.x == before.x);
    CHECK(after.y == before.y);
    CHECK(after.width == before.width);
    CHECK(after.height == before.height);
}

// -------------------------------------------------------------------------------------------
// @3x
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: FileAssetResolver picks @3x at >= 2.5, @2x at >= 1.5, and falls back") {
    const fs::path dir = xt::test::tempDir() / "xt_t345_3x";
    std::error_code ec;
    fs::remove_all(dir, ec);
    writeBytes(dir / "img.png", "abcd");
    writeBytes(dir / "img@2x.png", "abcdefgh");
    writeBytes(dir / "img@3x.png", "abcdefghijkl");
    writeBytes(dir / "two.png", "xy");
    writeBytes(dir / "two@2x.png", "wxyz");
    compile::IrWriter writer;
    for (const char* name : {"img.png", "two.png"}) {
        compile::AssetBuilder a;
        a.name = writer.intern(name);
        writer.addAsset(a);
    }
    compile::SceneBuilder scene;
    scene.width = 1;
    scene.height = 1;
    writer.addScene(scene);
    std::vector<uint8_t> blob = writer.finish();
    Result<IrView> view = IrView::open(IrBlob{blob.data(), blob.size()});
    REQUIRE(view.ok());
    FileAssetResolver resolver(dir.generic_string(), fakeDecode);
    CHECK(resolver.imageView(0, view.value(), 1.0f)->width == 4);
    CHECK(resolver.imageView(0, view.value(), 1.5f)->width == 8);
    CHECK(resolver.imageView(0, view.value(), 2.4f)->width == 8);
    CHECK(resolver.imageView(0, view.value(), 2.5f)->width == 12);
    CHECK(resolver.imageView(0, view.value(), 3.0f)->width == 12);
    CHECK(resolver.imageView(1, view.value(), 3.0f)->width == 4);  // No @3x: the @2x file.
    CHECK(resolver.imageSize(0, view.value()).x == 4.0f);
    fs::remove_all(dir, ec);
}

TEST_CASE("T-345: an image command carries its element's factor for the variant choice") {
    compile::CompileOptions options;
    options.emitMeta = false;
    const std::string text =
        "<xt><head><style>#i { position: absolute; } #j { position: absolute; top: 50px; ui-scale: none; }"
        "</style></head><body><img id=\"i\" src=\"x.png\"/><img id=\"j\" src=\"x.png\"/></body></xt>";
    const compile::SourceFile file{"doc.xt", text};
    compile::CompileResult result = compile::compileDocumentFromStrings(
        std::span<const compile::SourceFile>(&file, 1), "doc.xt", options);
    REQUIRE(result.ok());
    FixedSizeResolver assets;
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    REQUIRE(ctx.setUiScale(2.5f) == Status::kOk);
    ctx.update(0.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    // The intrinsic 40 x 20 is design px too.
    CHECK(doc->arena().element(doc->byId("i").node).box.width == doctest::Approx(100));
    CHECK(doc->arena().element(doc->byId("j").node).box.width == doctest::Approx(40));
    CommandList list;
    ctx.render(list);
    REQUIRE(list.images.size() == 2);
    std::vector<float> scales{list.images[0].variantScale, list.images[1].variantScale};
    std::sort(scales.begin(), scales.end());
    CHECK(scales[0] == doctest::Approx(1.0f));
    CHECK(scales[1] == doctest::Approx(2.5f));
}

// -------------------------------------------------------------------------------------------
// Pixel alignment only for static nodes
// -------------------------------------------------------------------------------------------

TEST_CASE("T-345: a text node under a moving parent is aligned to whole pixels only once it stops") {
    const std::string css =
        "@keyframes slide { from { transform: translateX(0px) } to { transform: translateX(3.5px) } } "
        "#p { position: absolute; left: 0; top: 0; width: 100px; height: 20px; "
        "animation: slide 10s linear forwards }";
    const std::vector<uint8_t> ir = compileDoc(css, R"(<div id="p">Hi</div>)");
    FakeTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0.0f);
    auto glyphX = [&ctx]() {
        CommandList list;
        ctx.render(list);
        REQUIRE(!list.glyphRuns.empty());
        return list.glyphRuns[0].xf.e;
    };
    ctx.update(2.5f);  // A quarter of the way: 0.875px.
    CHECK(glyphX() == doctest::Approx(0.875f));
    ctx.update(1.0f);  // Still moving: 1.225px, not snapped.
    CHECK(glyphX() == doctest::Approx(1.225f));
    ctx.update(10.0f);  // Finished (held at 3.5px): static again, so on a whole pixel.
    CHECK(glyphX() == doctest::Approx(4.0f));
    ctx.update(0.1f);
    CHECK(glyphX() == doctest::Approx(4.0f));
}

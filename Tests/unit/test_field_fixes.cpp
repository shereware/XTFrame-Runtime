#include <memory>
#include <string>

#include "doctest.h"
#include "style_decode.h"  // styleprop::* (internal; Code/Runtime/core/src is a test-only include dir).
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// Docs/tasks/T-119.md: the field-report fixes that need a whole compile -> load -> paint round
/// trip -- `opacity: 0` reaching NodeCommon (R1) and `visibility: hidden` covering an element's
/// image and its instantiated SVG scene (R2). The parser-level fixes live in test_xts_values.cpp
/// (animation/transition items, side longhands) and test_parse_svg.cpp (gradient defs/href).

using namespace xt;
using namespace xt::compile;

namespace {

std::string xtPath(const std::string& name) { return std::string(XT_TEST_XT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// R1: `opacity: 0` / `0.0` must reach NodeCommon::opacity and suppress the element's paint.
// ---------------------------------------------------------------------------------------------

TEST_CASE("T-119 R1: opacity 0 is a real value, not 'unset'") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_XT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("field_opacity_zero.xt", &shaper, &assets);

    struct Case {
        const char* id;
        float opacity;
    };
    const Case cases[] = {{"o0", 0.0f}, {"o0dot", 0.0f}, {"o001", 0.01f}, {"o05", 0.5f}, {"o1", 1.0f}};
    for (const Case& c : cases) {
        CAPTURE(c.id);
        ElementRef e = doc->byId(c.id);
        REQUIRE(e.valid());
        Value v = e.computedStyleValue(prop::Opacity);
        CHECK(v.type == ValueType::Float);
        CHECK(v.f == doctest::Approx(c.opacity));
        CHECK(e.property(prop::Opacity).f == doctest::Approx(c.opacity));
    }

    // The two zero-opacity boxes contribute no Fill at all; the other three do, each with its
    // own opacity folded into the paint alpha (one drawable item per box, so no Layer).
    CommandList cmds;
    doc->paint(cmds);
    REQUIRE(cmds.fills.size() == 3);
    CHECK(cmds.fills[0].paint.color.a == doctest::Approx(0.01f));
    CHECK(cmds.fills[1].paint.color.a == doctest::Approx(0.5f));
    CHECK(cmds.fills[2].paint.color.a == doctest::Approx(1.0f));
}

TEST_CASE("T-119 R1: runtime setStyle(\"opacity: 0\") takes effect too") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_XT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("field_opacity_zero.xt", &shaper, &assets);

    ElementRef e = doc->byId("o1");
    REQUIRE(e.valid());
    CHECK(e.property(prop::Opacity).f == doctest::Approx(1.0f));
    CHECK(e.setStyle("opacity: 0") == Status::kOk);
    doc->update(0.0f);
    CHECK(e.computedStyleValue(prop::Opacity).f == doctest::Approx(0.0f));
    CHECK(e.property(prop::Opacity).f == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------------------------
// R2: `visibility: hidden` covers an <img>'s raster draw and its instantiated SVG scene, whether
// declared on the element itself or inherited from an ancestor.
// ---------------------------------------------------------------------------------------------

TEST_CASE("T-119 R2: visibility hidden suppresses <img> and its SVG scene") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_XT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("field_img_visibility.xt", &shaper, &assets);

    CHECK(doc->byId("self").computedStyleValue(style_decode::styleprop::Visibility).e ==
          static_cast<uint32_t>(Visibility::Hidden));
    // Inherited from #hidingParent, which is the only one that declares it.
    CHECK(doc->byId("inherited").computedStyleValue(style_decode::styleprop::Visibility).e ==
          static_cast<uint32_t>(Visibility::Hidden));

    CommandList cmds;
    doc->paint(cmds);
    // Three raster <img>s, only #rasterShown visible.
    CHECK(cmds.images.size() == 1);
    // Three SVG scenes, only #shown visible; svg_icon.svg is one filled circle plus one stroked
    // path, so a painted scene contributes exactly one Fill and one Stroke.
    CHECK(cmds.fills.size() == 1);
    CHECK(cmds.strokes.size() == 1);
}

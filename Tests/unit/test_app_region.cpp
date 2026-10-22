#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "style_decode.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/node.h"

/// T-376 (Docs/design/05-style-and-css.md §5.9.10, 23-host-integration.md §23.ab): `app-region`
/// (StyleProp 275) parses none / drag / no-drag, is inherited, can be set at run time, and
/// Document::appRegionAt() walks the ancestors for the first value other than `none`.

using namespace xt;

namespace {

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"app_region.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "app_region.xt", compile::CompileOptions{});
}

struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;
    std::vector<uint8_t> ir;

    Loaded(const std::string& css, const std::string& body) {
        compile::CompileResult result = compileRaw(css, body);
        for (const compile::Diagnostic& d : result.diagnostics) {
            CAPTURE(d.message);
            CHECK(d.level != compile::Diagnostic::Level::Error);
        }
        REQUIRE(result.ok());
        ir = std::move(result.ir);
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.update(0.0f);
    }

    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    AppRegion region(const char* id) { return doc->arena().element(node(id)).style.appRegion; }
};

}  // namespace

TEST_CASE("T-376: app-region parses none, drag and no-drag") {
    Loaded l("#a{app-region:none} #b{app-region:drag} #c{app-region:no-drag}",
             R"(<div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div>)");
    CHECK(l.region("a") == AppRegion::None);
    CHECK(l.region("b") == AppRegion::Drag);
    CHECK(l.region("c") == AppRegion::NoDrag);
    CHECK(l.region("d") == AppRegion::None);  // The initial value.
    CHECK(style_decode::isInherited(style_decode::styleprop::AppRegion));
}

TEST_CASE("T-376: an unknown app-region value is rejected") {
    for (const char* bad : {"app-region: move", "app-region: drag no-drag", "app-region: 1"}) {
        CAPTURE(bad);
        const compile::CompileResult r = compileRaw(std::string("#a{") + bad + "}", R"(<div id="a"></div>)");
        CHECK_FALSE(r.diagnostics.empty());
    }
    Loaded l("", R"(<div id="a"></div>)");
    CHECK(l.doc->byId("a").setStyle("app-region: move") != Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.region("a") == AppRegion::None);
}

TEST_CASE("T-376: app-region is inherited; no-drag on a child overrides drag on its parent") {
    Loaded l("#bar{app-region:drag} #button{app-region:no-drag}",
             R"(<div id="bar"><div id="title"><span id="text">T</span></div>)"
             R"(<div id="button"><div id="icon"></div></div></div>)");
    CHECK(l.region("bar") == AppRegion::Drag);
    CHECK(l.region("title") == AppRegion::Drag);
    CHECK(l.region("text") == AppRegion::Drag);
    CHECK(l.region("button") == AppRegion::NoDrag);
    CHECK(l.region("icon") == AppRegion::NoDrag);
}

TEST_CASE("T-376: Document::appRegionAt walks the ancestor chain") {
    Loaded l("#bar{app-region:drag} #button{app-region:no-drag}",
             R"(<div id="bar"><div id="title"></div><div id="button"><div id="icon"></div></div></div>)"
             R"(<div id="content"><div id="inner"></div></div>)");
    CHECK(l.doc->appRegionAt(l.node("bar")) == AppRegion::Drag);
    CHECK(l.doc->appRegionAt(l.node("title")) == AppRegion::Drag);
    CHECK(l.doc->appRegionAt(l.node("button")) == AppRegion::NoDrag);
    CHECK(l.doc->appRegionAt(l.node("icon")) == AppRegion::NoDrag);
    CHECK(l.doc->appRegionAt(l.node("content")) == AppRegion::None);
    CHECK(l.doc->appRegionAt(l.node("inner")) == AppRegion::None);
    CHECK(l.doc->appRegionAt(kInvalidNode) == AppRegion::None);

    // A child set back to `none` at run time: the walk finds the ancestor's `drag`.
    REQUIRE(l.doc->byId("title").setStyle("app-region: none") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.region("title") == AppRegion::None);
    CHECK(l.doc->appRegionAt(l.node("title")) == AppRegion::Drag);  // From #bar.
}

TEST_CASE("T-376: setStyle app-region: drag at run time") {
    Loaded l("", R"(<div id="a"><div id="b"></div></div>)");
    CHECK(l.region("a") == AppRegion::None);
    REQUIRE(l.doc->byId("a").setStyle("app-region: drag") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.region("a") == AppRegion::Drag);
    CHECK(l.region("b") == AppRegion::Drag);  // Inherited.
    CHECK(l.doc->byId("a").computedStyleValue(style_decode::styleprop::AppRegion).e == 1u);
    REQUIRE(l.doc->byId("b").setStyle("app-region: no-drag") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.region("b") == AppRegion::NoDrag);
    CHECK(l.doc->appRegionAt(l.node("b")) == AppRegion::NoDrag);
}

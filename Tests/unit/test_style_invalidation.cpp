#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-723 (Docs/design/10-backends.md §10.16 W5 and its 補充裁決, Docs/tasks/T-723.md B): a state
/// change re-resolves only what the state dependency table says -- nothing, the element, or its
/// subtree -- and an element whose inherited part changed passes the change to its children.

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

std::unique_ptr<Document> loadFromString(const std::string& name, const std::string& css,
                                         const std::string& body, TextShaper& shaper) {
    const fs::path dir = xt::test::tempDir() / "xt_t723_style_invalidation";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (name + ".xt");
    const std::string source =
        "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    REQUIRE(file != nullptr);
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);

    CompileResult result = compileDocument(path.string(), CompileOptions{});
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

InputEvent pointerAt(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

bool styleDirty(Document& doc, const ElementRef& el) {
    return (doc.arena().dirty(el.node) & static_cast<uint8_t>(kDirtyStyle)) != 0;
}

constexpr const char* kBody =
    "<div id=\"outer\"><div id=\"panel\"><div id=\"child\"><div id=\"grand\"></div></div></div></div>";

}  // namespace

TEST_CASE("style invalidation (T-723 B.2): a subject-only :hover rule re-resolves the element alone") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("subject",
                                                   "#outer, #panel, #child, #grand { position: absolute; "
                                                   "left: 0px; top: 0px; width: 100px; height: 100px; }"
                                                   "#panel { background-color: #000000; color: #000000; }"
                                                   "#panel:hover { background-color: #ffffff; }",
                                                   kBody, shaper);
    ElementRef panel = doc->byId("panel");
    ElementRef child = doc->byId("child");
    ElementRef outer = doc->byId("outer");
    REQUIRE(panel.valid());
    doc->dispatchInput(pointerAt(Vec2{50, 50}));
    REQUIRE(panel.hasState(kStateHover));
    // `outer` and `grand` are hovered too, but no rule tests their hover: nothing for them; `panel`
    // alone (no ancestor, no descendant).
    CHECK(styleDirty(*doc, panel));
    CHECK(!styleDirty(*doc, child));
    CHECK(!styleDirty(*doc, outer));
    doc->update(0.0f);
    CHECK(panel.property(prop::BackgroundColor).c.r == doctest::Approx(1.0f));
}

TEST_CASE("style invalidation (T-723 F): an inherited change reaches the descendants") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("inherit",
                                                   "#outer, #panel, #child, #grand { position: absolute; "
                                                   "left: 0px; top: 0px; width: 100px; height: 100px; }"
                                                   "#panel { color: #000000; font-size: 10px; }"
                                                   "#panel:hover { color: #ffffff; font-size: 20px; }",
                                                   kBody, shaper);
    ElementRef panel = doc->byId("panel");
    ElementRef child = doc->byId("child");
    ElementRef grand = doc->byId("grand");
    REQUIRE(child.valid());
    CHECK(grand.property(prop::Color).c.r == doctest::Approx(0.0f));
    doc->dispatchInput(pointerAt(Vec2{50, 50}));
    CHECK(styleDirty(*doc, panel));
    CHECK(!styleDirty(*doc, child));  // Marked by the propagation rule during the resolve only.
    doc->update(0.0f);
    CHECK(child.property(prop::Color).c.r == doctest::Approx(1.0f));
    CHECK(grand.property(prop::Color).c.r == doctest::Approx(1.0f));
    CHECK(grand.property(prop::FontSize).len.value == doctest::Approx(20.0f));
    doc->dispatchInput(pointerAt(Vec2{300, 250}));
    doc->update(0.0f);
    CHECK(grand.property(prop::Color).c.r == doctest::Approx(0.0f));
    CHECK(grand.property(prop::FontSize).len.value == doctest::Approx(10.0f));
}

TEST_CASE("style invalidation (T-723 B.2): a :hover in an ancestor compound re-resolves the subtree") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("ancestor",
                                                   "#outer, #panel, #child, #grand { position: absolute; "
                                                   "left: 0px; top: 0px; width: 100px; height: 100px; }"
                                                   "#grand { background-color: #000000; }"
                                                   "#panel:hover #grand { background-color: #ffffff; }",
                                                   kBody, shaper);
    ElementRef panel = doc->byId("panel");
    ElementRef grand = doc->byId("grand");
    ElementRef outer = doc->byId("outer");
    REQUIRE(grand.valid());
    doc->dispatchInput(pointerAt(Vec2{50, 50}));
    CHECK(styleDirty(*doc, panel));
    CHECK(styleDirty(*doc, grand));
    CHECK(!styleDirty(*doc, outer));
    doc->update(0.0f);
    CHECK(grand.property(prop::BackgroundColor).c.r == doctest::Approx(1.0f));
    doc->dispatchInput(pointerAt(Vec2{300, 250}));
    doc->update(0.0f);
    CHECK(grand.property(prop::BackgroundColor).c.r == doctest::Approx(0.0f));
}

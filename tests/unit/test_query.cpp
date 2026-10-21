#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Document::query: the 05.3 selector subset parsed at runtime and matched in preorder.
/// See docs/design/05-style-and-css.md §5.3, docs/tasks/T-35.md.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
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

size_t count(Document& doc, std::string_view selector, Status expected = Status::kOk) {
    std::vector<ElementRef> out;
    CHECK(doc.query(selector, out) == expected);
    return out.size();
}

}  // namespace

TEST_CASE("query: tag, class, id, universal") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("query_basic.xt", &shaper, &assets);

    std::vector<ElementRef> out;
    REQUIRE(doc->query("div", out) == Status::kOk);
    // xt > body > (#x, .c, .a) -- `body`/`xt` are not divs.
    CHECK(out.size() == 3);
    for (const ElementRef& ref : out) {
        CHECK(ref.tag() == "div");
    }

    CHECK(count(*doc, ".a") == 2);
    CHECK(count(*doc, "#x") == 1);
    CHECK(count(*doc, "span") == 3);
    CHECK(count(*doc, ".nosuchclass") == 0);
    CHECK(count(*doc, "#nosuchid") == 0);
    // Every Element in the document: xt, body, #x, span.b, .c, span.b, .a, span, button.
    CHECK(count(*doc, "*") == 9);

    // The result is in document (preorder) order.
    REQUIRE(doc->query(".a", out) == Status::kOk);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id() == "x");
    CHECK(out[1].id().empty());
}

TEST_CASE("query: compound selectors and combinators") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("query_basic.xt", &shaper, &assets);

    CHECK(count(*doc, "div.a") == 2);
    CHECK(count(*doc, "div.a#x") == 1);
    CHECK(count(*doc, "div.a > span") == 2);  // One direct span child in each .a.
    CHECK(count(*doc, "div.a>span") == 2);    // Whitespace around `>` is optional.
    CHECK(count(*doc, ".a .b") == 2);         // Both .b spans descend from #x.
    CHECK(count(*doc, ".a > .c > .b") == 1);
    CHECK(count(*doc, ".c .b") == 1);
    CHECK(count(*doc, "body div span") == 3);
    CHECK(count(*doc, "span .b") == 0);
}

TEST_CASE("query: state pseudo-classes") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("query_basic.xt", &shaper, &assets);

    CHECK(count(*doc, ":hover") == 0);
    CHECK(count(*doc, "button:disabled") == 1);  // disabled="true" at load time (05.2).
    CHECK(count(*doc, ":checked") == 0);

    ElementRef x = doc->byId("x");
    REQUIRE(x.valid());
    x.setState(kStateChecked, true);
    CHECK(count(*doc, "#x:checked") == 1);
    CHECK(count(*doc, "div:checked") == 1);
    x.setState(kStateChecked, false);
    CHECK(count(*doc, ":checked") == 0);
}

TEST_CASE("query: invalid syntax") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("query_basic.xt", &shaper, &assets);

    std::vector<ElementRef> out;
    out.push_back(doc->root());
    CHECK(doc->query("div, span", out) == Status::kInvalidArgument);  // Groups: 05.3 lists them,
    CHECK(out.empty());                                               // T-35's query does not.
    CHECK(doc->query("", out) == Status::kInvalidArgument);
    CHECK(doc->query("> div", out) == Status::kInvalidArgument);
    CHECK(doc->query("div >", out) == Status::kInvalidArgument);
    CHECK(doc->query("div.", out) == Status::kInvalidArgument);
    CHECK(doc->query("#", out) == Status::kInvalidArgument);
    CHECK(doc->query("div:nosuch", out) == Status::kInvalidArgument);
    CHECK(doc->query(":not(.a)", out) == Status::kInvalidArgument);
    CHECK(doc->query("div::before", out) == Status::kInvalidArgument);
    CHECK(doc->query("[data-x]", out) == Status::kInvalidArgument);
    CHECK(doc->query("div + span", out) == Status::kInvalidArgument);
    CHECK(doc->query(".a div.b span", out) == Status::kOk);
}

TEST_CASE("query: sees runtime classes and dynamically created elements") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("query_basic.xt", &shaper, &assets);

    ElementRef x = doc->byId("x");
    REQUIRE(x.valid());
    CHECK(x.addClass("runtimeonly"));
    CHECK(count(*doc, ".runtimeonly") == 1);

    ElementRef made = doc->createElement("div");
    REQUIRE(made.valid());
    CHECK(made.setAttribute("id", "made") == Status::kOk);
    CHECK(count(*doc, "#made") == 0);  // Detached: not part of the tree yet.
    doc->appendChild(x, made);
    CHECK(count(*doc, "#made") == 1);
    CHECK(count(*doc, "#x > div") == 2);
    doc->remove(made);
    CHECK(count(*doc, "#made") == 0);
}

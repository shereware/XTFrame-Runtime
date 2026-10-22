#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-342 (Docs/design/03-scene-and-properties.md §3.13, 22-bundle.md §22.15): the bare
/// `editor-only` attribute and CompileOptions::stripEditorOnly.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr std::string_view kDoc =
    "<xt><head>"
    "<symbol name=\"Badge\"><div id=\"face\"></div><div id=\"guide\" editor-only></div></symbol>"
    "<style>#ref { width: 100px; height: 50px; } #hp { width: 20px; height: 10px; }</style>"
    "</head><body>"
    "<div id=\"ref\" editor-only><img id=\"shot\" src=\"references/shot.png\"/>"
    "<img id=\"both\" src=\"shared.png\"/></div>"
    "<div id=\"hp\"><img id=\"icon\" src=\"shared.png\"/></div>"
    "<use symbol=\"Badge\" id=\"badge\"/>"
    "</body></xt>";

CompileResult compileText(std::string_view text, bool strip) {
    std::array<SourceFile, 1> files{SourceFile{"editor_only.xt", text}};
    CompileOptions options;
    options.stripEditorOnly = strip;
    return compileDocumentFromStrings(files, "editor_only.xt", options);
}

std::unique_ptr<Document> load(const CompileResult& result, TextShaper& shaper) {
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

bool hasAsset(const CompileResult& result, std::string_view source) {
    for (const CompiledAsset& asset : result.assets) {
        if (asset.sourceName == source) {
            return true;
        }
    }
    return false;
}

std::string warnings(const CompileResult& result) {
    std::string out;
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning) {
            out += d.message + "\n";
        }
    }
    return out;
}

}  // namespace

TEST_CASE("T-342: editor-only may be written bare and is kept as an attribute") {
    DiagnosticSink sink;
    DomTree dom;
    REQUIRE(parseXt("<xt><body><img src=\"a.png\" editor-only/><div editor-only=\"\"></div></body></xt>",
                    "bare.xt", sink, dom));
    CHECK_FALSE(sink.hasErrors());
    int flagged = 0;
    for (const DomNode& node : dom.nodes) {
        if (const DomAttr* attr = node.attr("editor-only")) {
            CHECK(attr->value.empty());
            ++flagged;
        }
    }
    CHECK(flagged == 2);
}

TEST_CASE("T-342: without stripEditorOnly the editor-only subtree is compiled as usual") {
    const CompileResult result = compileText(kDoc, false);
    REQUIRE(result.ok());
    CHECK(result.strippedIds.empty());
    CHECK(hasAsset(result, "references/shot.png"));
    CHECK(hasAsset(result, "shared.png"));
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = load(result, shaper);
    CHECK(doc->byId("ref").valid());
    CHECK(doc->byId("shot").valid());
    CHECK(doc->byId("badge/guide").valid());
}

TEST_CASE("T-342: stripEditorOnly removes the subtree, its own assets and the symbol part") {
    const CompileResult result = compileText(kDoc, true);
    REQUIRE(result.ok());
    // Document order; the `<head>` symbol's editor-only part comes after the body.
    CAPTURE(result.strippedIds.size());
    CHECK(result.strippedIds == std::vector<std::string>{"ref", "shot", "both", "guide"});
    CHECK_FALSE(hasAsset(result, "references/shot.png"));  // Only the removed subtree named it.
    CHECK(hasAsset(result, "shared.png"));                 // #icon still does.
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = load(result, shaper);
    CHECK_FALSE(doc->byId("ref").valid());
    CHECK_FALSE(doc->byId("shot").valid());
    CHECK_FALSE(doc->byId("both").valid());
    CHECK(doc->byId("hp").valid());
    CHECK(doc->byId("icon").valid());
    CHECK(doc->byId("badge/face").valid());
    CHECK_FALSE(doc->byId("badge/guide").valid());  // Inside the symbol definition, removed too.
    CHECK(warnings(result).find("editor-only") == std::string::npos);
}

TEST_CASE("T-342: a remaining reference to a removed element is a warning") {
    const std::string text =
        "<xt><head><symbol name=\"Ghost\" editor-only><div id=\"g\"></div></symbol></head><body>"
        "<div id=\"ref\" editor-only></div>"
        "<div id=\"button\" on:click=\"toggleClass(#ref, on)\" data-other=\"#reference\"></div>"
        "<use symbol=\"Ghost\" id=\"ghost\"/>"
        "</body></xt>";
    const CompileResult result = compileText(text, true);
    const std::string w = warnings(result);
    CAPTURE(w);
    CHECK(w.find("editor-only element 'ref' is referenced by <div> attribute 'on:click'") !=
          std::string::npos);
    CHECK(w.find("attribute 'data-other'") == std::string::npos);  // `#reference` is another id.
    CHECK(w.find("editor-only element 'Ghost' is referenced by <use>") != std::string::npos);
    // The canvas view compiles both without a word about it.
    const CompileResult canvas = compileText(text, false);
    CHECK(warnings(canvas).find("editor-only") == std::string::npos);
}

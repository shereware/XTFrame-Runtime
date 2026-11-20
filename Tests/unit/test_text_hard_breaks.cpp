#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/text/ft_text_shaper.h"

/// T-801 (Docs/design/07-text.md §7.2 supplement): a host sets a bound text through a port with
/// hard line breaks and a TAB in it, on a shaper holding only the Tests/fonts subsets (no system
/// fonts, none of which has these codepoints). The text breaks into lines and the command list
/// draws no glyph 0 (.notdef).

using namespace xt;

namespace {

std::string fontPath(const char* name) { return std::string(XT_TEST_FONTS_DIR) + "/" + name; }

std::vector<uint8_t> compileBoundText() {
    const std::string source =
        "<xt><head><style>"
        "body { width: 200px; height: 200px; font-family: \"Noto Sans\"; font-size: 16px; color: #000000; }"
        "#msg { position: absolute; left: 4px; top: 4px; width: 180px; }"
        "</style></head><body id=\"Root\">"
        "<div id=\"hud\" port:msg=\"string\"></div>"
        "<div id=\"msg\" bind:text=\"msg\">start</div>"
        "</body></xt>";
    compile::CompileOptions options;
    options.emitMeta = false;
    std::vector<compile::SourceFile> files{{"main.xt", source}};
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

}  // namespace

TEST_CASE("Document: T-801 setPortText with hard breaks and a TAB draws no glyph 0") {
    text::FtTextShaper shaper;
    REQUIRE(shaper.addFontFile(fontPath("NotoSans-Regular.subset.ttf")));
    const std::vector<uint8_t> ir = compileBoundText();

    ContextConfig config;
    config.shaper = &shaper;
    Context ctx(config);
    ctx.setViewport(Rect{0, 0, 200, 200}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    const PortId msg = doc->lookupPort("msg");
    REQUIRE(msg != kInvalidPort);

    const std::vector<std::string> values = {
        "First\nSecond",
        "First\r\nSecond",
        "First\tSecond",
        "First\xE2\x80\xA8Second",  // U+2028
        "A\nB\r\nC\tD\xE2\x80\xA8"
        "E",
    };
    for (const std::string& value : values) {
        CAPTURE(value);
        REQUIRE(doc->setPortText(msg, value) == Status::kOk);
        ctx.update(0.0f);
        CommandList list;
        ctx.render(list, nullptr);
        REQUIRE(!list.glyphRuns.empty());
        REQUIRE(!list.glyphs.empty());
        for (uint32_t glyph : list.glyphs) {
            CHECK(glyph != 0u);
        }
    }
}

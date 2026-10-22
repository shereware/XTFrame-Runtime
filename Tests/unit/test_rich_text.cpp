#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/property.h"

/// T-178 (Docs/design/07-text.md §7.8): rich-text runs (parse, IR run table, measurement,
/// wrapping across runs), `text-reveal`, `text-effect` commands, `<glyph>` resolution and link
/// hit testing. Layout uses FakeTextShaper's monospace metrics: an ASCII glyph advances 0.6em, a
/// space 0.3em; ascent 0.8em, descent 0.2em; the initial line-height is 1.2em.

using namespace xt;
using namespace xt::compile;

namespace {

const std::string kObj = "\xEF\xBF\xBC";  // U+FFFC, an inline object's text.

std::string document(const std::string& css, const std::string& body) {
    return "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
}

CompileResult compileSource(const std::string& source) {
    SourceFile file{"rich.xt", source};
    CompileOptions options;
    return compileDocumentFromStrings(std::span<const SourceFile>(&file, 1), "rich.xt", options);
}

std::unique_ptr<Document> loadDoc(const std::string& source, TextShaper& shaper) {
    CompileResult result = compileSource(source);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

NodeId textNodeOf(Document& doc, ElementRef element) {
    NodeId found = kInvalidNode;
    doc.arena().forEachChild(element.node, [&](NodeId child) {
        if (found == kInvalidNode && doc.arena().kind(child) == NodeKind::Text) {
            found = child;
        }
    });
    REQUIRE(found != kInvalidNode);
    return found;
}

const DomNode* findNode(const DomTree& tree, std::string_view tag) {
    for (const DomNode& node : tree.nodes) {
        if (node.tag == tag) {
            return &node;
        }
    }
    return nullptr;
}

class MapGlyphProvider : public IGlyphProvider {
public:
    AssetRef resolve(std::string_view action, InputKind device) override {
        ++calls;
        if (action == "jump" && device == InputKind::Gamepad) {
            return AssetRef{"pad_a.png"};
        }
        return AssetRef{};
    }
    int calls = 0;
};

}  // namespace

TEST_CASE("rich text: parse flattens inline markup into one text with runs") {
    const std::string src =
        "<xt><body><p>Hello <b>big</b> <span style=\"color: #f00\" class=\"k\">red</span> "
        "<a on:click=\"go()\">link</a> <glyph key=\"jump\" height=\"12\"/> end</p></body></xt>";
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt(src, "rich.xt", sink, tree));
    // The inline elements are gone from the tree: xt, body, p.
    CHECK(tree.nodes.size() == 3);
    const DomNode* p = findNode(tree, "p");
    REQUIRE(p != nullptr);
    CHECK(p->hasText);
    CHECK(p->children.empty());
    CHECK(p->text == "Hello big red link " + kObj + " end");
    REQUIRE(p->textRuns.size() == 4);
    CHECK(p->textRuns[0].start == 6);
    CHECK(p->textRuns[0].end == 9);
    CHECK(p->textRuns[0].style.find("font-weight: bold") != std::string::npos);
    CHECK(p->textRuns[1].start == 10);
    CHECK(p->textRuns[1].end == 13);
    CHECK(p->textRuns[1].style.find("color: #f00") != std::string::npos);
    REQUIRE(p->textRuns[1].classes.size() == 1);
    CHECK(p->textRuns[1].classes[0] == "k");
    CHECK(p->textRuns[2].start == 14);
    CHECK(p->textRuns[2].end == 18);
    CHECK(p->textRuns[2].link == 0);
    REQUIRE(p->textLinks.size() == 1);
    CHECK(p->textLinks[0] == "go()");
    CHECK(p->textRuns[3].start == 19);
    CHECK(p->textRuns[3].end == 22);
    CHECK(p->textRuns[3].inlineKind == DomInlineKind::Glyph);
    CHECK(p->textRuns[3].inlineRef == "jump");
    CHECK(p->textRuns[3].inlineHeight == "12");
}

TEST_CASE("rich text: nested markup merges styles; block children still refuse text") {
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt("<xt><body><p>a <b>x <i>y</i></b> z</p></body></xt>", "n.xt", sink, tree));
    const DomNode* p = findNode(tree, "p");
    REQUIRE(p != nullptr);
    CHECK(p->text == "a x y z");
    REQUIRE(p->textRuns.size() == 2);
    CHECK(p->textRuns[0].start == 2);  // "x " (bold)
    CHECK(p->textRuns[0].end == 4);
    CHECK(p->textRuns[1].start == 4);  // "y" (bold italic)
    CHECK(p->textRuns[1].end == 5);
    CHECK(p->textRuns[1].style.find("font-weight: bold") != std::string::npos);
    CHECK(p->textRuns[1].style.find("font-style: italic") != std::string::npos);

    DiagnosticSink sink2;
    DomTree tree2;
    CHECK_FALSE(parseXt("<xt><body><p>a <div>x</div></p></body></xt>", "e.xt", sink2, tree2));

    // Elements without direct text keep their children as blocks.
    DiagnosticSink sink3;
    DomTree tree3;
    REQUIRE(parseXt("<xt><body><p><span>a</span><span>b</span></p></body></xt>", "b.xt", sink3, tree3));
    CHECK(tree3.nodes.size() == 5);
}

TEST_CASE("rich text: a standalone <glyph> is an element whose text is its inline glyph") {
    DiagnosticSink sink;
    DomTree tree;
    REQUIRE(parseXt("<xt><body><div><glyph key=\"jump\"/></div></body></xt>", "g.xt", sink, tree));
    const DomNode* glyph = findNode(tree, "glyph");
    REQUIRE(glyph != nullptr);
    CHECK(glyph->text == kObj);
    REQUIRE(glyph->textRuns.size() == 1);
    CHECK(glyph->textRuns[0].inlineKind == DomInlineKind::Glyph);
    CHECK(glyph->textRuns[0].inlineRef == "jump");
}

TEST_CASE("rich text: the IR run table keeps the override subset, class rules and objects") {
    CompileResult result = compileSource(document(".k { color: #0000ff; font-size: 20px; width: 10px; }",
                                                  "<p id=\"a\">x <b>y</b> <span class=\"k\">z</span> <a "
                                                  "on:click=\"go()\">w</a> <glyph key=\"jump\"/></p>"));
    REQUIRE(result.ok());
    bool warned = false;
    for (const Diagnostic& d : result.diagnostics) {
        warned = warned || d.message.find("rich-text runs only take") != std::string::npos;
    }
    CHECK(warned);  // `width` in `.k` is dropped for the run.
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    const DocChunk& doc = view.value().doc();
    std::span<const IrTextRun> runs = doc.textRuns();
    REQUIRE(runs.size() == 4);
    // <b>: font-weight 700 only.
    REQUIRE(runs[0].declCount == 1);
    CHECK(doc.inlineDecls()[runs[0].declOffset].prop == 173);
    CHECK(irValueU32(doc.inlineDecls()[runs[0].declOffset].value, 0) == 700);
    // .k: color + font-size (width dropped).
    CHECK(runs[1].declCount == 2);
    // <a>: a link, no declarations.
    CHECK(runs[2].declCount == 0);
    REQUIRE(runs[2].link < doc.textLinks().size());
    CHECK(view.value().string(doc.textLinks()[runs[2].link].onClick) == "go()");
    // <glyph>: an inline object.
    REQUIRE(runs[3].inlineObject < doc.textInlines().size());
    CHECK(doc.textInlines()[runs[3].inlineObject].kind == 1);
    CHECK(view.value().string(doc.textInlines()[runs[3].inlineObject].ref) == "jump");
}

TEST_CASE("rich text: a run is measured with its own font size; the line takes the largest") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(
        document("#a { font-size: 10px; }", "<p id=\"a\">ab<span style=\"font-size: 20px\">cd</span></p>"),
        shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = doc->arena().text(textNodeOf(*doc, a));
    REQUIRE(text.lines.size() == 1);
    // 2 x 6px + 2 x 12px.
    CHECK(text.lines[0].rect.w == doctest::Approx(36.0f));
    // line-height 1.2em of the 20px run.
    CHECK(text.height == doctest::Approx(24.0f));
    REQUIRE(text.runs.size() == 2);
    CHECK(text.runs[0].tag == kNoRunTag);
    CHECK(text.runs[1].tag == 0);
    CHECK(text.runs[1].fontSize == doctest::Approx(20.0f));
    CHECK(text.runs[1].positions[0].x == doctest::Approx(12.0f));
    // Both runs share the line's baseline: (24 - (16 + 4)) / 2 + 16 = 18.
    CHECK(text.runs[0].positions[0].y == doctest::Approx(18.0f));
    CHECK(text.runs[1].positions[0].y == doctest::Approx(18.0f));
}

TEST_CASE("rich text: lines break across runs at the whole text's break opportunities") {
    FakeTextShaper shaper;
    // "aaaa bbbb cccc" at 10px: 24 + 3 + 24 + 3 + 24 = 78px; 60px fits "aaaa bbbb" (51px).
    std::unique_ptr<Document> doc = loadDoc(
        document("#a { width: 60px; font-size: 10px; }", "<p id=\"a\">aaaa <b>bbbb</b> cccc</p>"), shaper);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    const TextNodeData& text = doc->arena().text(textNodeOf(*doc, a));
    REQUIRE(text.lines.size() == 2);
    CHECK(text.lines[0].byteStart == 0);
    CHECK(text.lines[1].byteStart == 10);
    bool boldOnFirstLine = false;
    for (uint32_t r = text.lines[0].firstRun; r < text.lines[0].firstRun + text.lines[0].runCount; ++r) {
        boldOnFirstLine = boldOnFirstLine || text.runs[r].tag == 0;
    }
    CHECK(boldOnFirstLine);
}

TEST_CASE("rich text: text-reveal shows the leading fraction of grapheme clusters") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc =
        loadDoc(document("#a { font-size: 10px; text-reveal: 0.5; }", "<p id=\"a\">ab<b>cd</b></p>"), shaper);
    CommandList list;
    doc->paint(list);
    CHECK(list.glyphs.size() == 2);
    REQUIRE(list.glyphs.size() == 2);
    CHECK(list.glyphs[0] == 'a');
    CHECK(list.glyphs[1] == 'b');

    ElementRef a = doc->byId("a");
    setProperty(doc->arena(), a.node, prop::TextReveal, Value::ofFloat(0.75f));
    doc->update(0.0f);
    list.clear();
    doc->paint(list);
    CHECK(list.glyphs.size() == 3);
}

TEST_CASE("rich text: text-effect becomes glyph-level commands under the fill") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(
        document(
            "#a { font-size: 10px; color: #fff; text-effect: outline 2px #000, shadow 1px 2px 3px #f00; }",
            "<p id=\"a\">ab</p>"),
        shaper);
    CommandList list;
    doc->paint(list);
    std::vector<const GlyphsCmd*> cmds;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::DrawGlyphs) {
            cmds.push_back(&list.glyphRuns[c.index]);
        }
    }
    REQUIRE(cmds.size() == 3);
    // Last listed first: shadow, outline, then the fill.
    CHECK(cmds[0]->effect == GlyphEffect::Blur);
    CHECK(cmds[0]->effectRadius == doctest::Approx(3.0f));
    CHECK(cmds[0]->xf.e - cmds[2]->xf.e == doctest::Approx(1.0f));
    CHECK(cmds[0]->xf.f - cmds[2]->xf.f == doctest::Approx(2.0f));
    CHECK(cmds[0]->color.r == doctest::Approx(1.0f));
    CHECK(cmds[1]->effect == GlyphEffect::Dilate);
    CHECK(cmds[1]->effectRadius == doctest::Approx(2.0f));
    CHECK(cmds[1]->color.r == doctest::Approx(0.0f));
    CHECK(cmds[2]->effect == GlyphEffect::None);
    // The effect commands reuse the fill's glyphs.
    CHECK(cmds[0]->glyphOffset == cmds[2]->glyphOffset);
}

TEST_CASE("rich text: <glyph> resolves through the provider and follows the input device") {
    FakeTextShaper shaper;
    ContextConfig config;
    config.shaper = &shaper;
    Context ctx(config);
    CompileResult result = compileSource(
        document("#a { font-size: 10px; } #pad { display: none; }",
                 "<img id=\"pad\" src=\"pad_a.png\"/><p id=\"a\">Press <glyph key=\"jump\"/> now</p>"));
    REQUIRE(result.ok());
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);
    ElementRef a = doc->byId("a");
    REQUIRE(a.valid());
    NodeId textNode = textNodeOf(*doc, a);

    // No provider: drawn as the text "[jump]" (six glyphs between "Press " and " now").
    {
        const TextNodeData& text = doc->arena().text(textNode);
        REQUIRE(text.inlines.size() == 1);
        CHECK(text.inlines[0].asset == kInvalidAsset);
        CHECK(text.inlines[0].box.w == doctest::Approx(0.0f));
        uint32_t glyphs = 0;
        for (const GlyphRun& run : text.runs) {
            glyphs += static_cast<uint32_t>(run.glyphs.size());
        }
        CHECK(glyphs == 6 + 6 + 4);
    }

    MapGlyphProvider provider;
    ctx.setGlyphProvider(&provider);
    CHECK(provider.calls == 1);
    ctx.update(0.0f);
    CHECK(doc->arena().text(textNode).inlines[0].asset == kInvalidAsset);  // Mouse: no glyph.

    REQUIRE(ctx.setInputDevice(InputKind::Gamepad) == Status::kOk);
    ctx.update(0.0f);
    {
        const TextNodeData& text = doc->arena().text(textNode);
        CHECK(text.inlines[0].asset != kInvalidAsset);
        // Square (no image size without a resolver), font-size high, on the baseline.
        CHECK(text.inlines[0].box.w == doctest::Approx(10.0f));
        CHECK(text.inlines[0].box.h == doctest::Approx(10.0f));
        CHECK(text.inlines[0].box.x == doctest::Approx(33.0f));  // "Press " = 5 x 6 + 3.
        CommandList list;
        doc->paint(list);
        CHECK(list.images.size() == 1);
    }

    REQUIRE(ctx.setInputDevice(InputKind::Mouse) == Status::kOk);
    ctx.update(0.0f);
    CHECK(doc->arena().text(textNode).inlines[0].asset == kInvalidAsset);
    ctx.setGlyphProvider(nullptr);
}

TEST_CASE("rich text: hitTestTextLink reports the <a> run under the point") {
    FakeTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc(
        document("#a { font-size: 10px; }", "<p id=\"a\">ab <a on:click=\"go()\">cd</a></p>"), shaper);
    // "ab " = 15px; "cd" spans x 15..27 on the first 12px line.
    CHECK(doc->hitTestTextLink(Vec2{20, 5}) == 0);
    CHECK(doc->hitTestTextLink(Vec2{5, 5}) == 0xFFFFFFFFu);
}

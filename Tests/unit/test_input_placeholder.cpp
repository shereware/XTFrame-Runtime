#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-395: `<input placeholder>` drawing (Docs/design/08-events.md §8.16): laid out apart from the
/// value (no caret position, no intrinsic size), painted in `--placeholder-color` or `color` at
/// 50 % alpha while the value is empty, focused or not; an `@key` placeholder follows the language.

using namespace xt;

namespace {

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"placeholder.xt", text};
    compile::CompileResult result =
        compile::compileDocumentFromStrings({&file, 1}, "placeholder.xt", compile::CompileOptions{});
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

class MapProvider : public ILocalizationProvider {
public:
    std::map<std::pair<std::string, std::string>, std::string> table;

    std::string_view resolve(std::string_view key, std::string_view lang) override {
        auto it = table.find({std::string(lang), std::string(key)});
        return it == table.end() ? std::string_view{} : std::string_view(it->second);
    }
};

InputEvent textInput(const char* utf8) {
    InputEvent ev;
    ev.kind = InputEvent::TextInput;
    const size_t n = std::strlen(utf8);
    REQUIRE(n < sizeof(ev.utf8));
    std::memcpy(ev.utf8, utf8, n);
    ev.utf8Len = static_cast<uint8_t>(n);
    return ev;
}

InputEvent keyDown(KeyCode key) {
    InputEvent ev;
    ev.kind = InputEvent::KeyDown;
    ev.key = key;
    return ev;
}

/// One Context + one Document in a 400 x 300 viewport, updated once.
struct Loaded {
    FakeTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;

    explicit Loaded(const std::vector<uint8_t>& ir, ILocalizationProvider* provider = nullptr,
                    const char* lang = nullptr) {
        ctx.setLocalizationProvider(provider);
        if (lang != nullptr) {
            REQUIRE(ctx.setLanguage(lang) == Status::kOk);
        }
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        ctx.update(0.0f);
    }

    ElementRef el(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref;
    }
    const TextNodeData& text(const char* id) {
        NodeId child = doc->arena().firstChild(el(id).node);
        while (child != kInvalidNode && doc->arena().kind(child) != NodeKind::Text) {
            child = doc->arena().nextSibling(child);
        }
        REQUIRE(child != kInvalidNode);
        return doc->arena().text(child);
    }
    size_t placeholderGlyphs(const char* id) {
        size_t n = 0;
        for (const GlyphRun& run : text(id).placeholderRuns) {
            n += run.glyphs.size();
        }
        return n;
    }
    /// Every DrawGlyphs command of a fresh paint, as (glyph count, color).
    std::vector<std::pair<uint32_t, Color>> glyphCommands() {
        CommandList list;
        doc->paint(list);
        std::vector<std::pair<uint32_t, Color>> out;
        for (const Command& cmd : list.commands) {
            if (cmd.kind == CmdKind::DrawGlyphs) {
                const GlyphsCmd& g = list.glyphRuns[cmd.index];
                out.emplace_back(g.glyphCount, g.color);
            }
        }
        return out;
    }
};

const char* kCss =
    "input { position: absolute; left: 10px; top: 10px; width: 200px; height: 20px; "
    "font-size: 10px; color: #204060; }";

}  // namespace

TEST_CASE("T-395: an empty input lays out its placeholder apart from the value") {
    Loaded l(compileDoc(kCss, "<input id=\"inp\" type=\"text\" placeholder=\"Search\"/>"));
    const TextNodeData& text = l.text("inp");
    CHECK(text.runs.empty());
    CHECK(l.placeholderGlyphs("inp") == 6);
    REQUIRE(text.lines.size() == 1);
    CHECK(text.lines[0].runCount == 0);  // The caret line stays the empty value's.

    // Painted in `color` at 50 % alpha.
    auto cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].first == 6);
    CHECK(cmds[0].second.r == doctest::Approx(0x20 / 255.0f));
    CHECK(cmds[0].second.b == doctest::Approx(0x60 / 255.0f));
    CHECK(cmds[0].second.a == doctest::Approx(0.5f));
}

TEST_CASE("T-395: focused it still shows, the caret sits at the start, typing hides it") {
    Loaded l(compileDoc(kCss, "<input id=\"inp\" type=\"text\" placeholder=\"Search\"/>"));
    Loaded plain(compileDoc(kCss, "<input id=\"inp\" type=\"text\"/>"));
    l.el("inp").focus();
    plain.el("inp").focus();
    l.ctx.update(0.0f);
    plain.ctx.update(0.0f);

    CHECK(l.placeholderGlyphs("inp") == 6);
    const Rect caret = l.doc->textInputRect();
    const Rect plainCaret = plain.doc->textInputRect();
    CHECK(caret.h > 0.0f);
    CHECK(caret.x == doctest::Approx(plainCaret.x));  // Not after the placeholder.
    auto cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].first == 6);

    l.doc->dispatchInput(textInput("a"));
    l.ctx.update(0.0f);
    CHECK(l.text("inp").placeholderRuns.empty());
    cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].first == 1);  // Only the typed "a".
    CHECK(cmds[0].second.a == doctest::Approx(1.0f));

    l.doc->dispatchInput(keyDown(KeyCode::Backspace));
    l.ctx.update(0.0f);
    CHECK(l.placeholderGlyphs("inp") == 6);  // Empty again: back.
}

TEST_CASE("T-395: a value from the start draws no placeholder; setAttribute changes it") {
    Loaded l(compileDoc(kCss,
                        "<input id=\"a\" type=\"text\" placeholder=\"Search\" value=\"hi\"/>"
                        "<input id=\"b\" type=\"text\"/>"));
    CHECK(l.text("a").placeholderRuns.empty());
    CHECK(l.text("b").placeholderRuns.empty());
    REQUIRE(l.el("b").setAttribute("placeholder", "Name") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.placeholderGlyphs("b") == 4);
    REQUIRE(l.el("b").setAttribute("placeholder", "") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.text("b").placeholderRuns.empty());
}

TEST_CASE("T-395: --placeholder-color wins over the 50 % color") {
    Loaded l(compileDoc(std::string(":root { --placeholder-color: #ff0000; } ") + kCss,
                        "<input id=\"inp\" type=\"text\" placeholder=\"Search\"/>"));
    auto cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].second.r == doctest::Approx(1.0f));
    CHECK(cmds[0].second.g == doctest::Approx(0.0f));
    CHECK(cmds[0].second.a == doctest::Approx(1.0f));

    // A setToken() override follows on the next paint.
    REQUIRE(l.ctx.setToken("--placeholder-color", Value::ofColor(Color{0, 0, 1, 1})) == Status::kOk);
    l.ctx.update(0.0f);
    cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].second.b == doctest::Approx(1.0f));
    CHECK(cmds[0].second.r == doctest::Approx(0.0f));
}

TEST_CASE("T-395: the placeholder does not change the input's intrinsic width") {
    const char* css = "input { position: absolute; font-size: 10px; }";
    Loaded with(compileDoc(css, "<input id=\"inp\" type=\"text\" placeholder=\"A long placeholder text\"/>"));
    Loaded without(compileDoc(css, "<input id=\"inp\" type=\"text\"/>"));
    CHECK(with.placeholderGlyphs("inp") > 0);
    CHECK(with.el("inp").layoutBox().width == doctest::Approx(without.el("inp").layoutBox().width));
}

TEST_CASE("T-395: an @key placeholder follows the language") {
    MapProvider provider;
    provider.table[{"en", "search_hint"}] = "Find";
    provider.table[{"de", "search_hint"}] = "Suchen bitte";
    Loaded l(compileDoc(kCss, "<input id=\"inp\" type=\"text\" placeholder=\"@search_hint\"/>"), &provider,
             "en");
    CHECK(l.el("inp").attribute("placeholder") == "Find");
    CHECK(l.placeholderGlyphs("inp") == 4);
    REQUIRE(l.ctx.setLanguage("de") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.placeholderGlyphs("inp") == 12);
    auto cmds = l.glyphCommands();
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].first == 12);
}

TEST_CASE("T-397: an unresolved @key placeholder draws nothing (no table, or the key is missing)") {
    const std::vector<uint8_t> ir =
        compileDoc(kCss, "<input id=\"inp\" type=\"text\" placeholder=\"@search_hint\"/>");
    Loaded none(ir);
    CHECK(none.text("inp").placeholderRuns.empty());
    CHECK(none.glyphCommands().empty());

    MapProvider provider;
    provider.table[{"en", "other_key"}] = "Other";
    provider.table[{"de", "search_hint"}] = "Suchen";
    Loaded missing(ir, &provider, "en");
    CHECK(missing.text("inp").placeholderRuns.empty());
    CHECK(missing.glyphCommands().empty());

    // The language that has the key draws it; back to one without it draws nothing again.
    REQUIRE(missing.ctx.setLanguage("de") == Status::kOk);
    missing.ctx.update(0.0f);
    CHECK(missing.placeholderGlyphs("inp") == 6);
    REQUIRE(missing.ctx.setLanguage("en") == Status::kOk);
    missing.ctx.update(0.0f);
    CHECK(missing.text("inp").placeholderRuns.empty());

    // A plain placeholder is unaffected.
    Loaded plain(compileDoc(kCss, "<input id=\"inp\" type=\"text\" placeholder=\"search_hint\"/>"));
    CHECK(plain.placeholderGlyphs("inp") == 11);
}

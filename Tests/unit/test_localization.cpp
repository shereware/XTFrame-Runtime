#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/layout.h"
#include "xt/types.h"

/// T-179: `@key` localization keys, the `STRS` chunk, ILocalizationProvider, Context::setLanguage()
/// and the `@font-fallback` chain. See Docs/design/07-text.md §7.8, 05-style-and-css.md §5.9.1,
/// 12-compiler.md §12.7, 13-api.md §13.8.2, Docs/tasks/T-179.md "測試".

using namespace xt;

namespace {

compile::CompileResult compileRaw(const std::string& css, const std::string& body,
                                  const std::string& head = std::string()) {
    const std::string text =
        "<xt><head>" + head + "<style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"l10n.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "l10n.xt", compile::CompileOptions{});
}

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body,
                                const std::string& head = std::string()) {
    compile::CompileResult result = compileRaw(css, body, head);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

bool hasError(const compile::CompileResult& r, const std::string& needle) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const compile::Diagnostic& d) {
        return d.level == compile::Diagnostic::Level::Error && d.message.find(needle) != std::string::npos;
    });
}

IrView openView(const std::vector<uint8_t>& ir) {
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()});
    REQUIRE(view.ok());
    return view.value();
}

/// The key string text node `textIndex` refers to, or "" when it has none.
std::string textKey(const IrView& view, size_t textIndex) {
    const LocalizationChunk& l10n = view.localization();
    if (textIndex >= l10n.textKeys().size() || l10n.textKeys()[textIndex] == 0) {
        return std::string();
    }
    return std::string(view.string(l10n.keys()[l10n.textKeys()[textIndex] - 1]));
}

/// A map-backed provider: (lang, key) -> text.
class MapProvider : public ILocalizationProvider {
public:
    std::map<std::pair<std::string, std::string>, std::string> table;
    uint32_t calls = 0;

    std::string_view resolve(std::string_view key, std::string_view lang) override {
        ++calls;
        auto it = table.find({std::string(lang), std::string(key)});
        return it == table.end() ? std::string_view{} : std::string_view(it->second);
    }
};

/// FakeTextShaper that records the family list of every shape() call.
class RecordingShaper : public FakeTextShaper {
public:
    std::vector<std::string> lastFamilies;

    void shape(std::string_view utf8, const ShapeParams& params, ShapeResult& out) override {
        if (utf8 != std::string_view("\xE2\x80\xA6")) {  // Skip the ellipsis probe.
            lastFamilies.clear();
            for (std::string_view family : params.font.families) {
                lastFamilies.emplace_back(family);
            }
        }
        FakeTextShaper::shape(utf8, params, out);
    }
};

/// One Context + one loaded Document laid out in a 400 x 300 viewport.
struct Loaded {
    RecordingShaper shaper;
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
    std::string text(const char* id) { return std::string(el(id).text()); }
    float width(const char* id) { return doc->arena().element(el(id).node).box.width; }
};

}  // namespace

TEST_CASE("T-179: @key text and attributes compile into STRS") {
    std::vector<uint8_t> ir = compileDoc("",
                                         "<div id=\"a\">@hud_hp</div>"
                                         "<img id=\"i\" alt=\"@img-alt\" title=\"plain\"/>"
                                         "<div id=\"b\">@hud_hp</div>"
                                         "<div id=\"c\">Hello</div>");
    IrView view = openView(ir);
    const LocalizationChunk& l10n = view.localization();
    REQUIRE(l10n.present());
    REQUIRE(l10n.keys().size() == 2);  // hud_hp once, img-alt.
    CHECK(view.string(l10n.keys()[0]) == "hud_hp");
    CHECK(view.string(l10n.keys()[1]) == "img-alt");
    REQUIRE(l10n.textKeys().size() == view.doc().texts().size());
    REQUIRE(l10n.attrKeys().size() == view.doc().attrPool().size());
    // The DOCT text is the bare key (what shows without a provider).
    CHECK(view.string(view.doc().texts()[0].text) == "hud_hp");
    CHECK(textKey(view, 0) == "hud_hp");
    CHECK(textKey(view, 1) == "hud_hp");
    CHECK(textKey(view, 2).empty());
    bool sawAlt = false;
    bool sawTitle = false;
    for (size_t i = 0; i < view.doc().attrPool().size(); ++i) {
        const IrAttr& attr = view.doc().attrPool()[i];
        if (view.string(attr.name) == "alt") {
            sawAlt = true;
            CHECK(view.string(attr.value) == "img-alt");
            CHECK(l10n.attrKeys()[i] == 2);
        } else if (view.string(attr.name) == "title") {
            sawTitle = true;
            CHECK(l10n.attrKeys()[i] == 0);
        }
    }
    CHECK(sawAlt);
    CHECK(sawTitle);
}

TEST_CASE("T-179: \\@ escapes a literal @, other attributes stay literal, no STRS without keys") {
    std::vector<uint8_t> ir = compileDoc("",
                                         "<div id=\"a\">\\@home</div>"
                                         "<div id=\"b\" data-x=\"@foo\" placeholder=\"\\@bar\">x</div>");
    IrView view = openView(ir);
    CHECK_FALSE(view.localization().present());
    CHECK(view.string(view.doc().texts()[0].text) == "@home");
    bool sawData = false;
    bool sawPlaceholder = false;
    for (const IrAttr& attr : view.doc().attrPool()) {
        if (view.string(attr.name) == "data-x") {
            sawData = true;
            CHECK(view.string(attr.value) == "@foo");
        } else if (view.string(attr.name) == "placeholder") {
            sawPlaceholder = true;
            CHECK(view.string(attr.value) == "@bar");
        }
    }
    CHECK(sawData);
    CHECK(sawPlaceholder);
}

TEST_CASE("T-179: an invalid key is a compile error") {
    CHECK(hasError(compileRaw("", "<div>@ hp</div>"), "invalid localization key"));
    CHECK(hasError(compileRaw("", "<div>@2x</div>"), "invalid localization key"));
    CHECK(hasError(compileRaw("", "<div>@</div>"), "invalid localization key"));
    CHECK(hasError(compileRaw("", "<div>@hud hp</div>"), "invalid localization key"));
    CHECK(hasError(compileRaw("", "<img alt=\"@a.b\"/>"), "invalid localization key"));
}

TEST_CASE("T-187: a key on text with rich-text runs warns that the translation invalidates them") {
    auto warns = [](const compile::CompileResult& r) {
        return std::count_if(r.diagnostics.begin(), r.diagnostics.end(), [](const compile::Diagnostic& d) {
            return d.level == compile::Diagnostic::Level::Warning &&
                   d.message.find("translation invalidates run offsets") != std::string::npos;
        });
    };
    const compile::CompileResult rich = compileRaw("", "<p id=\"a\">@title<b>x</b></p>");
    REQUIRE(rich.ok());
    CHECK(warns(rich) == 1);
    // A key without runs, and runs without a key, stay quiet.
    CHECK(warns(compileRaw("", "<p id=\"a\">@title</p>")) == 0);
    CHECK(warns(compileRaw("", "<p id=\"a\">title <b>x</b></p>")) == 0);
}

TEST_CASE("T-179: a slot: text fill can be a key") {
    const std::string head =
        "<symbol name=\"Btn\"><span id=\"Label\"><slot name=\"label\">Button</slot></span></symbol>";
    std::vector<uint8_t> ir =
        compileDoc("", "<use symbol=\"Btn\" id=\"P\" slot:label=\"@menu_play\"/>", head);
    IrView view = openView(ir);
    REQUIRE(view.localization().keys().size() == 1);
    CHECK(view.string(view.localization().keys()[0]) == "menu_play");

    MapProvider provider;
    provider.table[{"en", "menu_play"}] = "Play";
    Loaded l(ir, &provider, "en");
    CHECK(l.text("P/Label") == "Play");
}

TEST_CASE("T-179: STRS survives an IrWriter round trip, and a bad key index is rejected") {
    std::vector<uint8_t> ir =
        compileDoc("@font-fallback zh-TW { font-family: \"Noto Sans SC\", Extra Font; }",
                   "<div id=\"a\">@k1</div><div>@k2</div>");
    IrView view = openView(ir);
    CHECK(compile::IrWriter::fromView(view).finish() == ir);

    compile::IrWriter writer;
    compile::DocBuilder doc;
    IrDocElement root{};
    root.parent = 0xFFFFFFFFu;
    root.tag = writer.intern("xt");
    root.id = kInvalidString;
    root.scene = kInvalidScene;
    root.firstTextChild = 0;
    root.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(root);
    doc.texts.push_back(IrDocText{0, writer.intern("k")});
    writer.setDoc(doc);
    compile::LocalizationBuilder l10n;
    l10n.keys.push_back(writer.intern("k"));
    l10n.textKeys.push_back(2);  // Out of range: only one key.
    writer.setLocalization(l10n);
    std::vector<uint8_t> bad = writer.finish();
    CHECK(IrView::open(IrBlob{bad.data(), bad.size()}).status() == Status::kInvalidIr);

    l10n.textKeys = {1};
    writer.setLocalization(l10n);
    std::vector<uint8_t> good = writer.finish();
    CHECK(IrView::open(IrBlob{good.data(), good.size()}).ok());

    l10n.textKeys = {1, 0};  // Not parallel to texts[].
    writer.setLocalization(l10n);
    std::vector<uint8_t> bad2 = writer.finish();
    CHECK(IrView::open(IrBlob{bad2.data(), bad2.size()}).status() == Status::kInvalidIr);
}

TEST_CASE("T-179: the provider resolves keys; a missing one shows the key") {
    std::vector<uint8_t> ir = compileDoc("",
                                         "<div id=\"a\">@hud_hp</div><div id=\"b\">@missing</div>"
                                         "<img id=\"i\" alt=\"@img_alt\"/><div id=\"c\">Plain</div>");
    {
        Loaded l(ir);  // No provider at all.
        CHECK(l.text("a") == "hud_hp");
        CHECK(l.doc->localizationKey(l.el("a")) == "hud_hp");
        CHECK(l.doc->localizationKey(l.el("c")).empty());
    }
    MapProvider provider;
    provider.table[{"en", "hud_hp"}] = "HP";
    provider.table[{"en", "img_alt"}] = "Picture";
    provider.table[{"zh-TW", "hud_hp"}] = "生命值";
    provider.table[{"zh-TW", "img_alt"}] = "圖片";
    Loaded l(ir, &provider, "en");
    CHECK(l.ctx.language() == "en");
    CHECK(l.doc->language() == "en");
    CHECK(l.text("a") == "HP");
    CHECK(l.text("b") == "missing");
    CHECK(l.text("c") == "Plain");
    CHECK(l.el("i").attribute("alt") == "Picture");

    REQUIRE(l.ctx.setLanguage("zh-TW") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.text("a") == "生命值");
    CHECK(l.el("i").attribute("alt") == "圖片");
    CHECK(l.text("b") == "missing");

    // Clearing the provider shows the keys again.
    l.ctx.setLocalizationProvider(nullptr);
    CHECK(l.text("a") == "hud_hp");
    CHECK(l.el("i").attribute("alt") == "img_alt");

    CHECK(l.ctx.setLanguage("en US") == Status::kInvalidArgument);
    CHECK(l.ctx.language() == "zh-TW");
}

TEST_CASE("T-179: text the host replaced is left alone by a language switch") {
    std::vector<uint8_t> ir = compileDoc("", "<div id=\"a\">@hud_hp</div><div id=\"b\">@hud_hp</div>");
    MapProvider provider;
    provider.table[{"en", "hud_hp"}] = "HP";
    provider.table[{"de", "hud_hp"}] = "LP";
    Loaded l(ir, &provider, "en");
    l.el("b").setText("Custom");
    REQUIRE(l.ctx.setLanguage("de") == Status::kOk);
    CHECK(l.text("a") == "LP");
    CHECK(l.text("b") == "Custom");
    CHECK(l.doc->localizationKey(l.el("b")).empty());
}

TEST_CASE("T-179: a document loaded after setLanguage starts in that language; reload keeps it") {
    std::vector<uint8_t> ir = compileDoc("", "<div id=\"a\">@title</div>");
    MapProvider provider;
    provider.table[{"fr", "title"}] = "Titre";
    Loaded l(ir, &provider, "fr");
    CHECK(l.text("a") == "Titre");
    Result<DocumentHandle> reloaded = l.ctx.reloadDocument(1, IrBlob{ir.data(), ir.size()}, false);
    REQUIRE(reloaded.ok());
    Document* doc = l.ctx.document(reloaded.value());
    REQUIRE(doc != nullptr);
    CHECK(doc->byId("a").text() == "Titre");
}

TEST_CASE("T-179: switching language relays out the text") {
    std::vector<uint8_t> ir =
        compileDoc("#a { position: absolute; font-size: 10px; }", "<div id=\"a\">@k</div>");
    MapProvider provider;
    provider.table[{"en", "k"}] = "ab";
    provider.table[{"de", "k"}] = "abcdefgh";
    Loaded l(ir, &provider, "en");
    const float before = l.width("a");
    CHECK(before > 0.0f);
    REQUIRE(l.ctx.setLanguage("de") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("a") > before * 3.0f);
}

TEST_CASE("T-179: :lang() prefers the document language over the lang attribute") {
    std::vector<uint8_t> ir =
        compileDoc("div { width: 10px; } div:lang(zh) { width: 50px; }",
                   "<div id=\"a\" lang=\"en\"></div><div id=\"b\" lang=\"zh-TW\"></div>");
    Loaded l(ir);
    CHECK(l.width("a") == doctest::Approx(10.0f));
    CHECK(l.width("b") == doctest::Approx(50.0f));
    REQUIRE(l.ctx.setLanguage("zh-HK") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == doctest::Approx(50.0f));
    CHECK(l.width("b") == doctest::Approx(50.0f));
    REQUIRE(l.ctx.setLanguage("en") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("b") == doctest::Approx(10.0f));
    REQUIRE(l.ctx.setLanguage("") == Status::kOk);  // Back to the attributes.
    l.ctx.update(0.0f);
    CHECK(l.width("b") == doctest::Approx(50.0f));
}

TEST_CASE("T-179: @font-fallback appends the language's chain to the family list") {
    std::vector<uint8_t> ir = compileDoc(
        "@font-fallback zh { font-family: Generic CJK; }"
        "@font-fallback zh-TW { font-family: \"Noto Sans SC\", Extra; }"
        "@font-fallback ja { font-family: A; }"
        "@font-fallback ja { font-family: B; }"  // Replaces the first.
        "#a, #b { font-family: Base; }",
        "<div id=\"a\">text</div>");
    IrView view = openView(ir);
    REQUIRE(view.localization().fallbacks().size() == 3);

    Loaded l(ir);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base"});

    REQUIRE(l.ctx.setLanguage("zh-TW") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base", "Noto Sans SC", "Extra"});

    REQUIRE(l.ctx.setLanguage("zh-HK") == Status::kOk);  // Prefix match: the `zh` chain.
    l.ctx.update(0.0f);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base", "Generic CJK"});

    REQUIRE(l.ctx.setLanguage("ja") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base", "B"});

    REQUIRE(l.ctx.setLanguage("fr") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base"});
}

TEST_CASE("T-179: without a document language the lang attribute picks the chain") {
    std::vector<uint8_t> ir =
        compileDoc("@font-fallback zh-TW { font-family: CJK; } #a { font-family: Base; }",
                   "<div id=\"a\" lang=\"zh-TW\">text</div>");
    Loaded l(ir);
    CHECK(l.shaper.lastFamilies == std::vector<std::string>{"Base", "CJK"});
}

TEST_CASE("T-179: @font-fallback syntax errors") {
    auto parse = [](const std::string& css, compile::RuleSet& rules) {
        compile::DiagnosticSink sink;
        const bool ok = compile::parseXts(css, "t.xts", 1, sink, rules);
        return ok && !sink.hasErrors();
    };
    compile::RuleSet rules;
    CHECK(parse("@font-fallback zh-TW { font-family: \"Noto Sans SC\", Noto Sans, X; }", rules));
    REQUIRE(rules.fontFallbacks.size() == 1);
    CHECK(rules.fontFallbacks[0].lang == "zh-TW");
    CHECK(rules.fontFallbacks[0].families == std::vector<std::string>{"Noto Sans SC", "Noto Sans", "X"});

    compile::RuleSet r2;
    CHECK_FALSE(parse("@font-fallback zh { }", r2));
    compile::RuleSet r3;
    CHECK_FALSE(parse("@font-fallback { font-family: A; }", r3));
    compile::RuleSet r4;
    CHECK_FALSE(parse("@font-fallback zh { font-family: A,; }", r4));
    compile::RuleSet r5;
    CHECK_FALSE(parse("@font-fallback zh { font-family: 12px; }", r5));
}

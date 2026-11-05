#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-624: inline `<svg>` content (scene nodes, `<xt-star>`, a bound `<svg>`, a bound shape) and
/// `@key` text inside a `<repeat>` template paint and localize like the same content at document
/// level (Docs/design/27-interaction.md §27.3.4, 11 inline `<svg>`, 07 §7.8 / T-179).

using namespace xt;
using namespace xt::compile;

namespace {

CompileResult compileXt(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

std::vector<uint8_t> compileOk(const std::string& xt) {
    CompileResult r = compileXt(xt);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    return r.ir;
}

// The reporter's probe (XTAtrium, 2026-11-06), without its `<img>` (no asset resolver here): an
// inline `<svg>` with a bound display, one with an `<xt-star>`, a bound plain `div`, and the same
// bound `<svg>` at document level.
constexpr const char* kSvgProbe = R"~(<xt version="2">
  <ports>
    rows: list<struct{t: number, n: string}> in = [{"t":1,"n":"a"},{"t":0,"n":"b"}]
    k: number in = 1
  </ports>
  <body>
    <div id="root">
      <repeat id="list" port="rows">
        <div id="row">
          <svg id="s-bound" viewBox="0 0 22 22" bind:display="item(t) | eq(1)"><circle id="C" cx="11" cy="11" r="9" fill="#123a31"/></svg>
          <div id="box-bound" bind:display="item(t) | eq(1)"></div>
          <svg id="s-star" viewBox="0 0 22 22"><xt-star id="T" cx="11" cy="11" points="8" r="10" r2="7.6" round="0.2" fill="#b8322a"/></svg>
        </div>
      </repeat>
      <svg id="s-doc" viewBox="0 0 22 22" bind:display="k | eq(1)"><circle id="C" cx="11" cy="11" r="9" fill="#123a31"/></svg>
    </div>
  </body>
  <layout>
    #root { position: absolute; left: 0px; top: 0px; width: 400px; height: 200px; }
    #list { display: flex; flex-direction: column; gap: 4px; }
    #row { display: flex; flex-direction: row; gap: 6px; width: 300px; height: 30px; }
    #s-bound, #s-star, #s-doc { width: 22px; height: 22px; }
    #box-bound { width: 22px; height: 22px; }
  </layout>
  <style>
    #root { background-color: #f3eedf; }
    #box-bound { background-color: #e0a050; }
  </style>
</xt>)~";

constexpr int kW = 400;
constexpr int kH = 200;

struct Frame {
    std::vector<uint8_t> px;

    /// True when the pixel at `at` is within 24 per channel of (r, g, b).
    bool near(Vec2 at, int r, int g, int b) const {
        const int x = static_cast<int>(at.x);
        const int y = static_cast<int>(at.y);
        if (x < 0 || y < 0 || x >= kW || y >= kH) {
            return false;
        }
        const size_t o = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4;
        auto close = [](int a, int e) { return a - e <= 24 && e - a <= 24; };
        return close(px[o], r) && close(px[o + 1], g) && close(px[o + 2], b);
    }
};

Frame render(Document& doc) {
    CommandList list;
    doc.paint(list);
    Frame f;
    f.px.assign(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{f.px.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    return f;
}

std::unique_ptr<Document> loadDoc(const std::vector<uint8_t>& ir) {
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, kW, kH});
    doc->update(0.0f);
    return doc;
}

/// The border-box center of `id`.
Vec2 centerOf(const Document& doc, std::string_view id) {
    ElementRef e = doc.byId(id);
    REQUIRE(e.valid());
    const Rect r = e.boundingBox();
    return Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}

class MapProvider : public ILocalizationProvider {
public:
    std::map<std::pair<std::string, std::string>, std::string> table;

    std::string_view resolve(std::string_view key, std::string_view lang) override {
        auto it = table.find({std::string(lang), std::string(key)});
        return it == table.end() ? std::string_view{} : std::string_view(it->second);
    }
};

std::string textOf(const Document& doc, std::string_view id) {
    ElementRef e = doc.byId(id);
    REQUIRE(e.valid());
    return std::string(e.text());
}

}  // namespace

TEST_CASE("T-624: inline <svg> inside a <repeat> template paints like a document-level one") {
    const std::vector<uint8_t> ir = compileOk(kSvgProbe);
    std::unique_ptr<Document> doc = loadDoc(ir);
    REQUIRE(doc->repeatInstances("list").size() == 2);
    const Frame f = render(*doc);

    // The document-level bound <svg>'s circle and entry 0's bound plain div paint (control).
    CHECK(f.near(centerOf(*doc, "s-doc"), 0x12, 0x3a, 0x31));
    CHECK(f.near(centerOf(*doc, "list/0/box-bound"), 0xe0, 0xa0, 0x50));
    // Entry 0 (t = 1): the bound <svg>'s circle and the <xt-star> paint.
    CHECK(f.near(centerOf(*doc, "list/0/s-bound"), 0x12, 0x3a, 0x31));
    CHECK(f.near(centerOf(*doc, "list/0/s-star"), 0xb8, 0x32, 0x2a));
    // Entry 1 (t = 0): the bound <svg> is display: none; the unbound star still paints.
    CHECK(doc->byId("list/1/s-bound").boundingBox().w == 0.0f);
    CHECK(f.near(centerOf(*doc, "list/1/s-star"), 0xb8, 0x32, 0x2a));
}

TEST_CASE("T-624: a bound shape inside a <repeat> template's <svg> reads its own entry") {
    const std::vector<uint8_t> ir = compileOk(R"~(<xt version="2">
  <ports>
    rows: list<struct{c: color}> in = [{"c":"#ff0000"},{"c":"#0000ff"}]
  </ports>
  <body>
    <div id="root">
      <repeat id="list" port="rows">
        <svg id="s" viewBox="0 0 20 20"><rect id="R" x="0" y="0" width="20" height="20" fill="#00ff00" bind:fill="item(c)"/></svg>
      </repeat>
    </div>
  </body>
  <layout>
    #root { position: absolute; left: 0px; top: 0px; width: 400px; height: 200px; }
    #list { display: flex; flex-direction: column; }
    #s { width: 20px; height: 20px; }
  </layout>
</xt>)~");
    std::unique_ptr<Document> doc = loadDoc(ir);
    REQUIRE(doc->repeatInstances("list").size() == 2);
    const Frame f = render(*doc);
    CHECK(f.near(centerOf(*doc, "list/0/s"), 255, 0, 0));
    CHECK(f.near(centerOf(*doc, "list/1/s"), 0, 0, 255));
}

TEST_CASE("T-624: @key text inside a <repeat> template resolves and follows setLanguage") {
    const std::vector<uint8_t> ir = compileOk(R"~(<xt version="2">
  <ports>
    rows: list<struct{n: string}> in = [{"n":"a"},{"n":"b"}]
  </ports>
  <body>
    <div id="root">
      <div id="title">@menu_title</div>
      <repeat id="list" port="rows">
        <div id="row"><span id="label">@row_label</span><span id="name" bind:text="item(n)"></span></div>
      </repeat>
    </div>
  </body>
</xt>)~");
    MapProvider provider;
    provider.table[{"en", "menu_title"}] = "Menu";
    provider.table[{"en", "row_label"}] = "Row";
    provider.table[{"zh-TW", "menu_title"}] = "\xE9\x81\xB8\xE5\x96\xAE";  // 選單
    provider.table[{"zh-TW", "row_label"}] = "\xE5\x88\x97";               // 列
    const std::string zhRow = provider.table[{"zh-TW", "row_label"}];
    Context ctx{ContextConfig{nullptr, nullptr}};
    ctx.setLocalizationProvider(&provider);
    REQUIRE(ctx.setLanguage("zh-TW") == Status::kOk);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ctx.setViewport(Rect{0, 0, kW, kH}, 1.0f);
    ctx.update(0.0f);

    CHECK(textOf(*doc, "title") == provider.table[{"zh-TW", "menu_title"}]);
    REQUIRE(doc->repeatInstances("list").size() == 2);
    CHECK(textOf(*doc, "list/0/label") == zhRow);
    CHECK(textOf(*doc, "list/1/label") == zhRow);
    CHECK(textOf(*doc, "list/1/name") == "b");
    CHECK(doc->localizationKey(doc->byId("list/0/label")) == "row_label");

    REQUIRE(ctx.setLanguage("en") == Status::kOk);
    ctx.update(0.0f);
    CHECK(textOf(*doc, "title") == "Menu");
    CHECK(textOf(*doc, "list/0/label") == "Row");
    CHECK(textOf(*doc, "list/1/label") == "Row");

    // An entry instantiated after the switch shows the current language at once.
    REQUIRE(doc->setPortText(doc->lookupPort("rows"), R"([{"n":"a"},{"n":"b"},{"n":"c"}])") == Status::kOk);
    ctx.update(0.0f);
    REQUIRE(doc->repeatInstances("list").size() == 3);
    CHECK(textOf(*doc, "list/2/label") == "Row");

    // Text the host replaced is left alone by a later switch (T-179's rule, per instance).
    doc->byId("list/0/label").setText("custom");
    REQUIRE(ctx.setLanguage("zh-TW") == Status::kOk);
    ctx.update(0.0f);
    CHECK(textOf(*doc, "list/0/label") == "custom");
    CHECK(doc->localizationKey(doc->byId("list/0/label")).empty());
    CHECK(textOf(*doc, "list/1/label") == zhRow);
    CHECK(textOf(*doc, "list/2/label") == zhRow);
}

TEST_CASE("T-624: recycled virtualized instances keep their <svg> scene and @key text") {
    std::string rows = "[";
    for (int i = 0; i < 200; ++i) {
        rows += (i > 0 ? "," : "") + std::string("{\"n\":") + std::to_string(i) + "}";
    }
    rows += "]";
    const std::vector<uint8_t> ir = compileOk(R"~(<xt version="2">
  <ports>
    rows: list<struct{n: number}> in = []
  </ports>
  <body>
    <div id="Scroll" style="position: absolute; left: 0px; top: 0px; width: 200px; height: 200px; overflow: scroll">
      <repeat id="list" port="rows" virtualize>
        <div id="row" style="display: flex; height: 20px"><svg id="s" viewBox="0 0 20 20" style="width: 20px; height: 20px"><rect id="R" x="0" y="0" width="20" height="20" fill="#0000ff"/></svg><span id="label">@row_label</span></div>
      </repeat>
    </div>
  </body>
</xt>)~");
    MapProvider provider;
    provider.table[{"en", "row_label"}] = "Row";
    provider.table[{"de", "row_label"}] = "Zeile";
    Context ctx{ContextConfig{nullptr, nullptr}};
    ctx.setLocalizationProvider(&provider);
    REQUIRE(ctx.setLanguage("en") == Status::kOk);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    ctx.setViewport(Rect{0, 0, kW, kH}, 1.0f);
    REQUIRE(doc->setPortText(doc->lookupPort("rows"), rows) == Status::kOk);
    float clock = 0.0f;
    auto scrollTo = [&](float y) {
        setProperty(doc->arena(), doc->byId("Scroll").node, prop::ScrollY, Value::ofFloat(y));
        clock += 0.016f;
        ctx.update(clock);
    };
    scrollTo(0);
    for (int i = 0; i < 4; ++i) {
        scrollTo(i % 2 == 0 ? 3000.0f : 400.0f);
    }
    scrollTo(3000);
    REQUIRE(doc->byId("list/150/label").valid());
    CHECK(textOf(*doc, "list/150/label") == "Row");
    REQUIRE(ctx.setLanguage("de") == Status::kOk);
    scrollTo(400);  // Recycled rows come back from the pool in the new language.
    CHECK(textOf(*doc, "list/25/label") == "Zeile");
    const Frame f = render(*doc);
    CHECK(f.near(centerOf(*doc, "list/25/s"), 0, 0, 255));
    CHECK(f.near(centerOf(*doc, "list/26/s"), 0, 0, 255));
}

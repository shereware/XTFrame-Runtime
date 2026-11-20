#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "style_invalidation.h"  // Code/Runtime/core/src: StyleInvalidationTestAccess.
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-763 (Docs/tasks/T-763.md items 1-4, 10 §10.16 W5): the state-write paths T-723 left on the
/// old full range (the element, every ancestor and every descendant) re-resolve only what the
/// state dependency table says can change -- nothing, the element, or its subtree. Each path: the
/// same value re-resolves nothing, a change only the elements it can reach. Counted with
/// FrameStats::styledElements; the legacy mode (StyleInvalidationTestAccess::setLegacy, the old
/// full range) shows what the path re-resolved before.

using namespace xt;
using namespace xt::compile;

namespace {

/// Body > #Wrap > (#A.card > #Pad.cpad > #Lbl) + #B.plain + #F (focusable) + #P (the ports).
constexpr const char* kStyle =
    "#Wrap { display: flex; } #A, #B, #F { width: 40px; height: 20px; }"
    "#A:disabled { opacity: 0.5; }"
    "#B:hover { opacity: 0.25; }"
    "#B:state(Selected) { opacity: 0.75; }"
    ".card:state(Open) .cpad { opacity: 0.5; }"
    ".card .cpad { height: 10px; }"
    ".hot { opacity: 0.3; }";
constexpr const char* kBody =
    "<div id=\"P\" port:on=\"bool in\" port:sel=\"bool in\"></div>"
    "<div id=\"Wrap\">"
    "<div id=\"A\" class=\"card\"><div id=\"Pad\" class=\"cpad\"><span id=\"Lbl\">x</span></div></div>"
    "<div id=\"B\" class=\"plain\" bind:state.Checked=\"on\" bind:state.Selected=\"sel\"></div>"
    "<button id=\"F\">f</button>"
    "</div>";

struct Ui {
    std::unique_ptr<Context> ctx = std::make_unique<Context>(ContextConfig{});
    DocumentHandle handle = 0;
    float clock = 0;

    Document& doc() { return *ctx->document(handle); }
    ElementRef el(std::string_view id) { return doc().byId(id); }
    /// One update; the number of elements it re-resolved.
    uint32_t styled() {
        clock += 1.0f / 60.0f;
        ctx->update(1.0f / 60.0f);
        return ctx->perf().current().styledElements;
    }
    void port(std::string_view name, bool on) {
        const PortId p = doc().lookupPort(name);
        REQUIRE(p != kInvalidPort);
        REQUIRE(doc().setPortText(p, on ? "true" : "false") == Status::kOk);
    }
};

std::unique_ptr<Ui> open(bool legacy = false) {
    auto ui = std::make_unique<Ui>();
    const std::string text =
        std::string("<xt><head><style>") + kStyle + "</style></head>\n<body>" + kBody + "</body></xt>";
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", text}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    std::string diagnostics;
    for (const auto& d : result.diagnostics) {
        diagnostics += d.message + "; ";
    }
    CAPTURE(diagnostics);
    REQUIRE(result.ok());
    Result<DocumentHandle> loaded = ui->ctx->loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(loaded.ok());
    ui->handle = loaded.value();
    ui->ctx->setViewport(Rect{0, 0, 320, 240}, 1.0f);
    ui->ctx->perf().setEnabled(true);
    ui->ctx->perf().setDetail(1);
    StyleInvalidationTestAccess::setLegacy(ui->doc(), legacy);
    ui->styled();
    REQUIRE(ui->styled() == 0u);  // Settled.
    return ui;
}

}  // namespace

TEST_CASE("T-763 ElementRef::setState / setAttribute(disabled|checked): by the state dependency table") {
    auto ui = open();
    ElementRef a = ui->el("A");
    CHECK(a.setState(kStateDisabled, true));
    CHECK(ui->styled() == 1u);  // `#A:disabled`: #A alone, no ancestor, no descendant.
    CHECK_FALSE(a.setState(kStateDisabled, true));
    CHECK(ui->styled() == 0u);  // The same value.
    CHECK(a.setAttribute("disabled", "false") == Status::kOk);
    CHECK(ui->styled() == 1u);
    CHECK(ui->el("Pad").setAttribute("checked", "true") == Status::kOk);
    CHECK(ui->styled() == 0u);  // No rule tests `:checked`.

    auto legacy = open(/*legacy=*/true);
    CHECK(legacy->el("A").setState(kStateDisabled, true));
    CHECK(legacy->styled() == 6u);  // Before: #A, #Pad, #Lbl, #Wrap, body, the root.
}

TEST_CASE("T-763 setCustomState(Hover|...) built-in alias: by the state dependency table") {
    auto ui = open();
    REQUIRE(ui->doc().setCustomState(ui->el("B"), "Hover", true) == Status::kOk);
    CHECK(ui->styled() == 1u);
    REQUIRE(ui->doc().setCustomState(ui->el("B"), "Hover", true) == Status::kOk);
    CHECK(ui->styled() == 0u);
    REQUIRE(ui->doc().setCustomState(ui->el("A"), "Active", true) == Status::kOk);
    CHECK(ui->styled() == 0u);  // No rule tests `:active`.
}

TEST_CASE("T-763 custom states: subject, ancestor position, untested name, unchanged value") {
    auto ui = open();
    REQUIRE(ui->doc().setCustomState(ui->el("B"), "Selected", true) == Status::kOk);
    CHECK(ui->styled() == 1u);  // `#B:state(Selected)`.
    REQUIRE(ui->doc().setCustomState(ui->el("B"), "Selected", true) == Status::kOk);
    CHECK(ui->styled() == 0u);
    // `.card:state(Open) .cpad`: an ancestor position -- #A and its subtree (#Pad, #Lbl).
    REQUIRE(ui->doc().setCustomState(ui->el("A"), "Open", true) == Status::kOk);
    CHECK(ui->styled() == 3u);
    CHECK(ui->el("Pad").property(prop::Opacity).f == doctest::Approx(0.5f));
    REQUIRE(ui->doc().setCustomState(ui->el("A"), "Open", false) == Status::kOk);
    CHECK(ui->styled() == 3u);
    CHECK(ui->el("Pad").property(prop::Opacity).f == doctest::Approx(1.0f));
    // `Open` on an element no `.card` compound can match.
    REQUIRE(ui->doc().setCustomState(ui->el("B"), "Open", true) == Status::kOk);
    CHECK(ui->styled() == 0u);

    auto legacy = open(/*legacy=*/true);
    REQUIRE(legacy->doc().setCustomState(legacy->el("B"), "Selected", true) == Status::kOk);
    CHECK(legacy->styled() == 4u);  // Before: #B, #Wrap, body, the root.
}

TEST_CASE("T-763 bind:state.Checked / bind:state.X: by the state dependency table") {
    auto ui = open();
    ui->port("on", true);
    CHECK(ui->styled() == 0u);  // `bind:state.Checked`: no rule tests `:checked`.
    CHECK(ui->el("B").hasState(kStateChecked));
    ui->port("sel", true);
    CHECK(ui->styled() == 1u);  // `bind:state.Selected` (a custom state): #B alone.
    CHECK(ui->doc().hasCustomState(ui->el("B"), "Selected"));
    ui->port("sel", true);
    CHECK(ui->styled() == 0u);

    auto legacy = open(/*legacy=*/true);
    legacy->port("on", true);
    CHECK(legacy->styled() == 4u);  // Before: #B, #Wrap, body, the root.
}

TEST_CASE("T-763 :focus-player(): a focus move with no rule testing it re-resolves nothing") {
    auto ui = open();
    ui->doc().setFocus(ui->el("F"));
    CHECK(ui->styled() == 0u);
    ui->doc().setFocus(ElementRef{});
    CHECK(ui->styled() == 0u);

    auto legacy = open(/*legacy=*/true);
    legacy->doc().setFocus(legacy->el("F"));
    CHECK(legacy->styled() == 4u);  // Before: #F, #Wrap, body, the root.
}

TEST_CASE("T-763 classes: setAttribute(class) / addClass / removeClass by where the class is named") {
    auto ui = open();
    CHECK(ui->el("B").setAttribute("class", "plain hot") == Status::kOk);
    CHECK(ui->styled() == 1u);  // `.hot` is a subject class.
    CHECK(ui->el("B").property(prop::Opacity).f == doctest::Approx(0.3f));
    CHECK(ui->el("B").setAttribute("class", "hot plain") == Status::kOk);
    CHECK(ui->styled() == 0u);  // The same set.
    CHECK(ui->el("B").addClass("nowhere"));
    CHECK(ui->styled() == 0u);  // No rule names it.
    CHECK(ui->el("A").removeClass("card"));
    CHECK(ui->styled() == 3u);  // `.card .cpad`: an ancestor class -- #A and its subtree.
    CHECK(ui->el("Pad").layoutBox().height == doctest::Approx(0.0f));
    CHECK(ui->el("A").addClass("card"));
    CHECK(ui->styled() == 3u);
    CHECK(ui->el("Pad").layoutBox().height == doctest::Approx(10.0f));

    auto legacy = open(/*legacy=*/true);
    CHECK(legacy->el("B").addClass("nowhere"));
    CHECK(legacy->styled() == 4u);  // Before: #B, #Wrap, body, the root.
}

TEST_CASE("T-763 restoreState: each node by the narrow paths") {
    auto ui = open();
    // On #A, which no binding writes (restoreState() sets the ports first: #B's `bind:state.Selected`
    // would write its state from the port before the snapshot's states are applied).
    REQUIRE(ui->doc().setCustomState(ui->el("A"), "Open", true) == Status::kOk);
    CHECK(ui->styled() == 3u);
    const std::string snap = ui->doc().snapshotState();
    REQUIRE(ui->doc().setCustomState(ui->el("A"), "Open", false) == Status::kOk);
    CHECK(ui->styled() == 3u);
    REQUIRE(ui->doc().restoreState(snap) == Status::kOk);
    CHECK(ui->styled() == 3u);  // #A's state back: #A and its subtree, no ancestor.
    CHECK(ui->doc().hasCustomState(ui->el("A"), "Open"));
    // Nothing differs, but restoreState() clears every id'd element's states before setting the
    // listed ones (Document::applyState, `exact`): #A goes to none and back, each by the narrow
    // path -- its subtree again, still no ancestor (T-763 item 4: no special handling).
    REQUIRE(ui->doc().restoreState(snap) == Status::kOk);
    CHECK(ui->styled() == 3u);
}

TEST_CASE("T-763 setStyle: the element alone, its subtree for an inherited property") {
    auto ui = open();
    REQUIRE(ui->el("A").setStyle("width: 40px") == Status::kOk);
    CHECK(ui->styled() == 1u);
    REQUIRE(ui->el("A").setStyle("color: #ff0000") == Status::kOk);
    CHECK(ui->styled() == 3u);  // Inherited: #A, #Pad, #Lbl.
    CHECK(ui->el("Lbl").computedStyleValue(prop::Color).c.r == doctest::Approx(1.0f));
}

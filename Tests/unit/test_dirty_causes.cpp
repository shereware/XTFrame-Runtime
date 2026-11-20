#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-747 (Docs/design/57-measurement.md §57.3 items 1-4, Docs/design/23-host-integration.md §23.ag
/// items 1-2): every invalidating write carries a DirtyCause, the frame's cause table counts them,
/// the Document's visual generation moves at the write, the Painter says when its list depends on
/// the clock, and FrameStats::idlePaintMs is the paint + damage time of frames nothing asked for.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head>\n<body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

struct Ui {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    ElementRef el(std::string_view id) { return doc().byId(id); }
    PortId port(std::string_view name) { return doc().lookupPort(name); }
    /// One frame; returns its record.
    FrameStats frame(float dt = 1.0f / 60.0f) {
        ctx->update(dt);
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    /// `write` between this frame's update and render, then one more frame; the two records' cause
    /// tables summed (a write lands in the frame it is made in, the work it causes in the next).
    FrameStats step(const std::function<void()>& write) {
        ctx->update(1.0f / 60.0f);
        write();
        ctx->render(list, &damage);
        FrameStats sum = ctx->perf().current();
        const FrameStats next = frame();
        for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
            for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
                sum.causes[c][k] += next.causes[c][k];
            }
        }
        return sum;
    }
};

CompileResult compileText(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

std::unique_ptr<Ui> open(std::string_view style, std::string_view body) {
    auto ui = std::make_unique<Ui>();
    CompileResult result = compileText(page(style, body));
    test::requireCompiled(result);
    ui->ir = result.ir;
    ContextConfig config;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    ui->ctx->perf().setEnabled(true);
    ui->ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> handle = ui->ctx->loadDocument(IrBlob{ui->ir.data(), ui->ir.size()}, false);
    REQUIRE(handle.ok());
    ui->handle = handle.value();
    for (int i = 0; i < 4; ++i) {
        ui->frame();
    }
    return ui;
}

uint32_t allCauses(const FrameStats& f) {
    uint32_t sum = 0;
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
        sum += causeTotal(f, static_cast<DirtyCause>(c));
    }
    return sum;
}

/// `f`'s cause table counts `expected` and nothing else.
void checkOnly(const FrameStats& f, DirtyCause expected) {
    CAPTURE(dirtyCauseName(expected));
    CHECK(causeTotal(f, expected) > 0);
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
        const auto cause = static_cast<DirtyCause>(c);
        if (cause != expected) {
            CAPTURE(dirtyCauseName(cause));
            CHECK(causeTotal(f, cause) == 0);
        }
    }
}

constexpr const char* kBox = "#x { width: 40px; height: 20px; background-color: #336699; }";

}  // namespace

TEST_CASE("dirty causes: a @keyframes animation counts as Animation and moves the generation") {
    auto ui = open(std::string(kBox) +
                       "@keyframes pulse { from { opacity: 0.2 } to { opacity: 1 } }"
                       "#x { animation: pulse 1s infinite; }",
                   "<div id=\"x\"></div>");
    const uint64_t before = ui->doc().visualGeneration();
    const FrameStats f = ui->frame();
    checkOnly(f, DirtyCause::Animation);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: a timeline group counts as Timeline") {
    auto ui = open(std::string(kBox) + "@timeline intro { #x { 0s { opacity: 0 } 2s { opacity: 1 } } }",
                   "<div id=\"x\"></div>");
    REQUIRE(ui->ctx->playTimeline("", "intro") == Status::kOk);
    ui->frame();
    const uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->frame(), DirtyCause::Timeline);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: a running transition counts as Transition") {
    auto ui =
        open(std::string(kBox) + "#x { opacity: 1; transition: opacity 1s linear; } #x.dim { opacity: 0.2; }",
             "<div id=\"x\"></div>");
    REQUIRE(ui->el("x").addClass("dim"));
    ui->frame();
    ui->frame();
    const uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->frame(), DirtyCause::Transition);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: a port write and its binding count as Binding") {
    auto ui = open(kBox, "<div id=\"x\" port:hp=\"number\" bind:width=\"hp\"></div>");
    const uint64_t before = ui->doc().visualGeneration();
    const FrameStats f =
        ui->step([&] { REQUIRE(ui->doc().setPort(ui->port("hp"), Value::ofFloat(77)) == Status::kOk); });
    checkOnly(f, DirtyCause::Binding);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: a flow sink's write counts as Flow") {
    // xt_flow's sinks run their Document writes inside DirtyCauseScope(Flow) (flow_bindings.cpp's
    // applyResolvedInput); the same scope here.
    auto ui = open(kBox, "<div id=\"x\">a</div>");
    const uint64_t before = ui->doc().visualGeneration();
    const FrameStats f = ui->step([&] {
        DirtyCauseScope scope(ui->doc().arena(), DirtyCause::Flow);
        ui->el("x").setText("flowing");
    });
    checkOnly(f, DirtyCause::Flow);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: setStyle, setText and setCustomState count as HostApi") {
    auto ui =
        open(std::string(kBox) + "#x:state(Lit) { background-color: #ff0000; }", "<div id=\"x\">a</div>");
    uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] { REQUIRE(ui->el("x").setStyle("width: 60px") == Status::kOk); }),
              DirtyCause::HostApi);
    CHECK(ui->doc().visualGeneration() > before);
    before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] { ui->el("x").setText("b"); }), DirtyCause::HostApi);
    CHECK(ui->doc().visualGeneration() > before);
    before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] { REQUIRE(ui->doc().setCustomState(ui->el("x"), "Lit", true) == Status::kOk); }),
              DirtyCause::HostApi);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: hover counts as Input") {
    auto ui = open(std::string(kBox) + "#x:hover { background-color: #ff0000; }", "<div id=\"x\"></div>");
    const uint64_t before = ui->doc().visualGeneration();
    InputEvent move;
    move.kind = InputEvent::PointerMove;
    move.pos = Vec2{10, 10};
    ui->ctx->pushInput(move);
    FrameStats f = ui->frame();
    const FrameStats next = ui->frame();
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
        for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
            f.causes[c][k] += next.causes[c][k];
        }
    }
    checkOnly(f, DirtyCause::Input);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: the viewport size and a token count as Viewport") {
    auto ui =
        open("#x { width: 50%; height: 20px; background-color: var(--c, #336699); }", "<div id=\"x\"></div>");
    uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] { ui->ctx->setViewport(Rect{0, 0, 500, 300}, 1.0f); }), DirtyCause::Viewport);
    CHECK(ui->doc().visualGeneration() > before);
    before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] {
        REQUIRE(ui->ctx->setToken("--c", Value::ofColor(Color{1, 0, 0, 1})) == Status::kOk);
    }),
              DirtyCause::Viewport);
    CHECK(ui->doc().visualGeneration() > before);
}

TEST_CASE("dirty causes: an external texture bound again counts as External") {
    auto ui = open("#E { position: absolute; left: 0; top: 0; }",
                   "<external id=\"E\" source=\"engine://minimap\"></external>");
    const NodeId ext = ui->doc().externalOf(ui->el("E").node);
    REQUIRE(ext != kInvalidNode);
    REQUIRE(ui->doc().bindTexture(ext, 7, 64, 32) == Status::kOk);
    ui->frame();
    ui->frame();
    const uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] { REQUIRE(ui->doc().bindTexture(ext, 8, 64, 32) == Status::kOk); }),
              DirtyCause::External);
    CHECK(ui->doc().visualGeneration() > before);
    CHECK(ui->list.clockDependent);  // T-747: every external texture counts as clock-dependent.
}

TEST_CASE("dirty causes: a reload and repeat entries count as Load") {
    auto ui =
        open(std::string(kBox),
             "<div id=\"P\" port:items=\"list<struct{name: string}>\"></div>"
             "<div id=\"L\"><repeat port=\"items\" id=\"Item\"><div class=\"cell\"></div></repeat></div>");
    uint64_t before = ui->doc().visualGeneration();
    checkOnly(ui->step([&] {
        REQUIRE(ui->doc().setPortText(ui->port("items"), R"([{"name":"A"},{"name":"B"}])") == Status::kOk);
    }),
              DirtyCause::Load);
    CHECK(ui->doc().visualGeneration() > before);
    checkOnly(ui->step([&] {
        REQUIRE(ui->ctx->reloadDocument(ui->handle, IrBlob{ui->ir.data(), ui->ir.size()}, false).ok());
    }),
              DirtyCause::Load);
    CHECK(ui->doc().visualGeneration() > 0);
}

TEST_CASE("dirty causes: a static document writes nothing, keeps its generation and paints idle") {
    auto ui = open(kBox, "<div id=\"x\">static</div>");
    const uint64_t before = ui->doc().visualGeneration();
    for (int i = 0; i < 3; ++i) {
        const FrameStats f = ui->frame();
        CHECK(allCauses(f) == 0);
        CHECK(f.idlePaintMs > 0.0f);
        CHECK_FALSE(ui->list.clockDependent);
    }
    CHECK(ui->doc().visualGeneration() == before);
}

TEST_CASE("dirty causes: a focused input's caret makes the list clock-dependent, never idle") {
    auto ui = open("#i { width: 100px; height: 20px; }", "<input id=\"i\" type=\"text\" value=\"abc\"/>");
    ui->doc().setFocus(ui->el("i"));
    ui->frame();
    ui->frame();
    const uint64_t before = ui->doc().visualGeneration();
    for (int i = 0; i < 3; ++i) {
        const FrameStats f = ui->frame(0.3f);  // The caret blinks; nothing is written.
        CHECK(ui->list.clockDependent);
        CHECK(f.idlePaintMs == 0.0f);
    }
    CHECK(ui->doc().visualGeneration() == before);
}

TEST_CASE("dirty causes: detail level 1 records the nodes written per cause") {
    auto ui = open(kBox, "<div id=\"x\">a</div>");
    ui->ctx->perf().setDetail(1);
    ui->ctx->update(1.0f / 60.0f);
    ui->el("x").setText("b");
    const FrameStats& f = ui->ctx->perf().current();
    const uint32_t* nodes = f.causeNodes[static_cast<uint32_t>(DirtyCause::HostApi)];
    // T-755: setText() writes the Text node (the element is reached through its propagation).
    const NodeId text = ui->doc().arena().firstChild(ui->el("x").node);
    bool found = false;
    for (uint32_t i = 0; i < kDirtyCauseNodeSlots; ++i) {
        found = found || nodes[i] == ui->el("x").node || nodes[i] == text;
    }
    CHECK(found);
    CHECK(f.causeNodes[static_cast<uint32_t>(DirtyCause::Animation)][0] == kInvalidNode);
    ui->ctx->render(ui->list, &ui->damage);
}

TEST_CASE("dirty causes: the visual-generation check reports a write that moved no generation") {
    auto ui = open(kBox, "<div id=\"x\"></div>");
    ui->ctx->setVerifyVisualGeneration(true);  // Counts instead of aborting (the suites' env switch).
    ui->frame();
    ui->frame();
    CHECK(ui->ctx->visualCheck().frames > 0);
    CHECK(ui->ctx->visualCheck().mismatches == 0);
    // A write behind the cause bookkeeping's back: the Painter reads the stored color directly.
    ui->doc().arena().element(ui->el("x").node).style.base[prop::BackgroundColor] =
        Value::ofColor(Color{1, 0, 0, 1});
    ui->frame();
    CHECK(ui->ctx->visualCheck().mismatches == 1);
    CHECK(ui->ctx->visualCheck().first.find("#x") != std::string::npos);
}

TEST_CASE("dirty causes: xt_tests runs every Context with the visual-generation check") {
    Context ctx{ContextConfig{}};
    CHECK(ctx.verifyVisualGeneration());  // Tests/common/test_visual_check.h (Tests/unit/test_main.cpp)
}

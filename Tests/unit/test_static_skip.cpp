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

/// T-748 (Docs/design/23-host-integration.md §23.ag items 3-5): Context::render() skips the paint of
/// a Document whose visual generation did not move since its last paint (and whose list held nothing
/// clock-dependent, under unchanged paint options, with the previous list handed back): it reuses the
/// previous commands, reports no damage for it and counts FrameStats::paintSkipped. The verify mode
/// paints anyway and reports a difference as FrameStats::staticSkipMismatch.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head>\n<body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

std::vector<uint8_t> compileOk(std::string_view style, std::string_view body) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string text = page(style, body);  // SourceFile views it.
    return test::compileMainOk(text, options);
}

std::string dump(const CommandList& list) {
    std::string out;
    dumpCommandList(list, out);
    return out;
}

struct Ui {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<std::vector<uint8_t>> irs;
    std::vector<DocumentHandle> handles;
    CommandList list;
    DamageList damage;

    Document& doc(size_t i = 0) { return *ctx->document(handles[i]); }
    ElementRef el(std::string_view id, size_t i = 0) { return doc(i).byId(id); }
    /// One frame; returns its record.
    FrameStats frame(float dt = 1.0f / 60.0f) {
        ctx->update(dt);
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    /// `write` between this frame's update and render; returns that frame's record.
    FrameStats step(const std::function<void()>& write, float dt = 1.0f / 60.0f) {
        ctx->update(dt);
        write();
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    /// What painting Document `i` alone gives right now (the reference a reused list must equal).
    std::string fresh(size_t i = 0) {
        CommandList ref;
        doc(i).paint(ref);
        return dump(ref);
    }
    bool damageEmpty() const { return !damage.full && damage.rects.empty(); }
};

std::unique_ptr<Ui> open(std::string_view style, std::string_view body, int warmup = 4) {
    auto ui = std::make_unique<Ui>();
    ui->irs.push_back(compileOk(style, body));
    ContextConfig config;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    // xt_tests constructs every Context in the verify mode (paints anyway); these cases test the
    // skip itself, the verify-mode cases turn it back on.
    ui->ctx->setVerifyStaticSkip(false);
    ui->ctx->perf().setEnabled(true);
    ui->ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> handle =
        ui->ctx->loadDocument(IrBlob{ui->irs[0].data(), ui->irs[0].size()}, false);
    REQUIRE(handle.ok());
    ui->handles.push_back(handle.value());
    for (int i = 0; i < warmup; ++i) {
        ui->frame();
    }
    return ui;
}

/// Frames until `ui` is static again (paintSkipped), at most `limit`; returns how many it took.
int settle(Ui& ui, int limit = 30) {
    for (int i = 1; i <= limit; ++i) {
        if (ui.frame().paintSkipped == 1) {
            return i;
        }
    }
    return -1;
}

/// The checks of every write kind: the frame of the write paints (no skip) and shows what a fresh
/// paint shows (a restyle lands at the next update); once static again the frames are skipped, with
/// empty damage, and still show what a fresh paint shows.
void checkWrite(Ui& ui, const std::function<void()>& write, bool betweenUpdateAndRender = true) {
    REQUIRE(settle(ui) > 0);
    FrameStats f;
    if (betweenUpdateAndRender) {
        f = ui.step(write);
    } else {
        write();
        f = ui.frame();
    }
    CHECK(f.paintSkipped == 0);
    CHECK(dump(ui.list) == ui.fresh());
    // Some writes settle over a frame or two (a restyle that starts nothing): static afterwards.
    REQUIRE(settle(ui) > 0);
    const std::string shown = ui.fresh();
    for (int i = 0; i < 2; ++i) {
        const FrameStats s = ui.frame();
        CHECK(s.paintSkipped == 1);
        CHECK(s.paintMs == 0.0f);
        CHECK(ui.damageEmpty());
        CHECK(dump(ui.list) == shown);
    }
}

constexpr const char* kBox = "#x { width: 40px; height: 20px; background-color: #336699; }";

}  // namespace

TEST_CASE("static skip: a static document skips its paint from the second frame on") {
    auto ui = open(kBox, "<div id=\"x\">static</div>", /*warmup=*/0);
    CHECK(ui->frame().paintSkipped == 0);  // The first frame paints.
    const std::string shown = dump(ui->list);
    for (int i = 0; i < 3; ++i) {
        const void* before = ui->list.commands.data();
        const FrameStats f = ui->frame();
        CHECK(f.paintSkipped == 1);
        CHECK(f.paintMs == 0.0f);
        CHECK(f.idlePaintMs < 0.05f);
        CHECK(ui->damageEmpty());
        CHECK(ui->list.structureChecked);
        CHECK(dump(ui->list) == shown);
        // Every Document skipped: the previous list itself is handed out again, nothing is copied.
        CHECK(ui->list.commands.data() == before);
    }
}

TEST_CASE("static skip: turned off, every frame paints again") {
    auto ui = open(kBox, "<div id=\"x\">static</div>");
    CHECK(ui->frame().paintSkipped == 1);
    ui->ctx->setStaticSkipEnabled(false);
    CHECK_FALSE(ui->ctx->staticSkipEnabled());
    for (int i = 0; i < 3; ++i) {
        const FrameStats f = ui->frame();
        CHECK(f.paintSkipped == 0);
        CHECK(f.paintMs > 0.0f);
        CHECK(ui->damageEmpty());
    }
    ui->ctx->setStaticSkipEnabled(true);
    CHECK(ui->frame().paintSkipped == 1);
}

TEST_CASE("static skip: without a damage list (no previous list kept) every frame paints") {
    auto ui = open(kBox, "<div id=\"x\">static</div>");
    for (int i = 0; i < 3; ++i) {
        ui->ctx->update(1.0f / 60.0f);
        ui->ctx->render(ui->list);
        CHECK(ui->ctx->perf().current().paintSkipped == 0);
    }
}

TEST_CASE("static skip: each kind of write paints its frame, then the document skips again") {
    SUBCASE("a finite @keyframes animation") {
        auto ui = open(std::string(kBox) +
                           "@keyframes grow { from { width: 40px } to { width: 80px } }"
                           "#x.go { animation: grow 0.1s linear; }",
                       "<div id=\"x\"></div>");
        checkWrite(*ui, [&] { REQUIRE(ui->el("x").addClass("go")); });
    }
    SUBCASE("a timeline group") {
        auto ui = open(std::string(kBox) + "@timeline intro { #x { 0s { opacity: 0 } 0.1s { opacity: 1 } } }",
                       "<div id=\"x\"></div>");
        // Played before the update that evaluates it (between update and render it changes nothing
        // the paint reads yet: that frame is rightly skipped).
        checkWrite(
            *ui, [&] { REQUIRE(ui->ctx->playTimeline("", "intro") == Status::kOk); },
            /*betweenUpdateAndRender=*/false);
    }
    SUBCASE("a transition") {
        auto ui = open(std::string(kBox) +
                           "#x { opacity: 1; transition: opacity 0.1s linear; } #x.dim { opacity: 0.2; }",
                       "<div id=\"x\"></div>");
        checkWrite(*ui, [&] { REQUIRE(ui->el("x").addClass("dim")); });
    }
    SUBCASE("a port write through its binding") {
        auto ui = open(kBox, "<div id=\"x\" port:hp=\"number\" bind:width=\"hp\"></div>");
        checkWrite(*ui, [&] {
            REQUIRE(ui->doc().setPort(ui->doc().lookupPort("hp"), Value::ofFloat(77)) == Status::kOk);
        });
    }
    SUBCASE("a flow sink's write between update and render") {
        auto ui = open(kBox, "<div id=\"x\">a</div>");
        checkWrite(*ui, [&] {
            DirtyCauseScope scope(ui->doc().arena(), DirtyCause::Flow);
            ui->el("x").setText("flowing");
        });
    }
    SUBCASE("the host's setStyle between update and render") {
        auto ui = open(kBox, "<div id=\"x\"></div>");
        checkWrite(*ui, [&] { REQUIRE(ui->el("x").setStyle("width: 60px") == Status::kOk); });
    }
    SUBCASE("the host's setText and setCustomState") {
        auto ui =
            open(std::string(kBox) + "#x:state(Lit) { background-color: #ff0000; }", "<div id=\"x\">a</div>");
        checkWrite(*ui, [&] { ui->el("x").setText("b"); });
        checkWrite(*ui, [&] { REQUIRE(ui->doc().setCustomState(ui->el("x"), "Lit", true) == Status::kOk); });
    }
    SUBCASE("hover") {
        auto ui = open(std::string(kBox) + "#x:hover { background-color: #ff0000; }", "<div id=\"x\"></div>");
        checkWrite(
            *ui,
            [&] {
                InputEvent move;
                move.kind = InputEvent::PointerMove;
                move.pos = Vec2{10, 10};
                ui->ctx->pushInput(move);
            },
            /*betweenUpdateAndRender=*/false);
    }
    SUBCASE("a token") {
        auto ui = open("#x { width: 40px; height: 20px; background-color: var(--c, #336699); }",
                       "<div id=\"x\"></div>");
        checkWrite(*ui, [&] {
            REQUIRE(ui->ctx->setToken("--c", Value::ofColor(Color{1, 0, 0, 1})) == Status::kOk);
        });
    }
    SUBCASE("repeat entries and a reload") {
        auto ui = open(
            std::string(kBox) + ".cell { width: 10px; height: 10px; background-color: #ff0000; }",
            "<div id=\"P\" port:items=\"list<struct{name: string}>\"></div>"
            "<div id=\"L\"><repeat port=\"items\" id=\"Item\"><div class=\"cell\"></div></repeat></div>");
        checkWrite(*ui, [&] {
            REQUIRE(ui->doc().setPortText(ui->doc().lookupPort("items"), R"([{"name":"A"},{"name":"B"}])") ==
                    Status::kOk);
        });
        REQUIRE(settle(*ui) > 0);
        ui->ctx->update(1.0f / 60.0f);
        REQUIRE(ui->ctx->reloadDocument(ui->handles[0], IrBlob{ui->irs[0].data(), ui->irs[0].size()}, false)
                    .ok());
        ui->ctx->render(ui->list, &ui->damage);
        CHECK(ui->ctx->perf().current().paintSkipped == 0);
        CHECK(dump(ui->list) == ui->fresh());
        CHECK(settle(*ui) > 0);
    }
}

TEST_CASE("static skip: an animation that finished and holds at forwards skips from the second frame on") {
    auto ui = open(std::string(kBox) +
                       "@keyframes fade { from { opacity: 1 } to { opacity: 0.25 } }"
                       "#x { animation: fade 0.1s linear forwards; }",
                   "<div id=\"x\"></div>", /*warmup=*/0);
    // The frame the last keyframe value is reached; the next update finishes the instance (it writes
    // the held value once more and AnimationEnd restyles its element after the Animator ran); the
    // update after that applies the held value over the restyle; from the second frame after the
    // finishing one nothing is written any more.
    int reached = -1;
    for (int i = 0; i < 12 && reached < 0; ++i) {
        ui->frame();
        if (getProperty(ui->doc().arena(), ui->el("x").node, prop::Opacity).f == 0.25f) {
            reached = i;
        }
    }
    REQUIRE(reached >= 0);
    CHECK(ui->frame().paintSkipped == 0);  // Finishes.
    CHECK(ui->frame().paintSkipped == 0);  // The first frame after it: applied over the restyle.
    const uint64_t held = ui->doc().visualGeneration();
    const std::string shown = ui->fresh();
    for (int i = 0; i < 4; ++i) {
        const FrameStats f = ui->frame();
        CAPTURE(i);
        CHECK(f.paintSkipped == 1);
        CHECK(f.animEvaluated == 0);
        CHECK(f.activeAnimations == 1);
        CHECK(dump(ui->list) == shown);
    }
    CHECK(ui->doc().visualGeneration() == held);
    // A restyle of the element applies the held value again (and shows it).
    checkWrite(*ui, [&] { REQUIRE(ui->el("x").setStyle("width: 70px") == Status::kOk); });
    CHECK(getProperty(ui->doc().arena(), ui->el("x").node, prop::Opacity).f == 0.25f);
}

TEST_CASE("static skip: a sticky override stops writing once applied") {
    auto ui = open(kBox, "<div id=\"x\"></div>");
    ui->el("x").setProperty(prop::Opacity, Value::ofFloat(0.5f), /*sticky=*/true);
    ui->frame();  // Applies the override.
    const uint64_t held = ui->doc().visualGeneration();
    for (int i = 0; i < 3; ++i) {
        CHECK(ui->frame().paintSkipped == 1);
    }
    CHECK(ui->doc().visualGeneration() == held);
    // Overridden again, and over a restyle of its element.
    checkWrite(*ui, [&] { ui->el("x").setProperty(prop::Opacity, Value::ofFloat(0.75f), /*sticky=*/true); });
    checkWrite(*ui, [&] { REQUIRE(ui->el("x").setStyle("opacity: 0.1; width: 50px") == Status::kOk); });
    CHECK(getProperty(ui->doc().arena(), ui->el("x").node, prop::Opacity).f == 0.75f);
}

TEST_CASE("static skip: clock-dependent content is never skipped") {
    SUBCASE("a focused input's caret") {
        auto ui = open("#i { width: 100px; height: 20px; }", "<input id=\"i\" type=\"text\" value=\"abc\"/>");
        ui->doc().setFocus(ui->el("i"));
        for (int i = 0; i < 4; ++i) {
            CHECK(ui->frame(0.3f).paintSkipped == 0);
        }
    }
    SUBCASE("an external texture") {
        auto ui = open("#E { position: absolute; left: 0; top: 0; }",
                       "<external id=\"E\" source=\"engine://minimap\"></external>");
        const NodeId ext = ui->doc().externalOf(ui->el("E").node);
        REQUIRE(ext != kInvalidNode);
        REQUIRE(ui->doc().bindTexture(ext, 7, 64, 32) == Status::kOk);
        for (int i = 0; i < 4; ++i) {
            CHECK(ui->frame().paintSkipped == 0);
        }
    }
}

TEST_CASE("static skip: a change of viewport, pixel ratio, paint option or list is never skipped") {
    auto ui = open(kBox, "<div id=\"x\"></div>");
    REQUIRE(settle(*ui) > 0);
    ui->ctx->setViewport(Rect{0, 0, 500, 300}, 1.0f);
    CHECK(ui->frame().paintSkipped == 0);
    REQUIRE(settle(*ui) > 0);
    ui->ctx->setViewport(Rect{0, 0, 500, 300}, 2.0f);
    CHECK(ui->frame().paintSkipped == 0);
    REQUIRE(settle(*ui) > 0);
    ui->ctx->setBoxShadowCommands(false);
    CHECK(ui->frame().paintSkipped == 0);
    REQUIRE(settle(*ui) > 0);
    ui->ctx->setHostTargetMode(HostTargetMode::Direct);
    CHECK(ui->frame().paintSkipped == 0);
    REQUIRE(settle(*ui) > 0);
    // Another list handed back (a copy carries the serial, so it is still the base) / a cleared one.
    CommandList copy = ui->list;
    ui->list.clear();
    CHECK(ui->frame().paintSkipped == 0);
    CHECK(ui->damage.full);
    ui->list = copy;
    CHECK(ui->frame().paintSkipped == 0);  // Not the list of the previous render() any more.
    CHECK(settle(*ui) > 0);
}

TEST_CASE("static skip: the verify mode paints, compares and reports a write that moved no generation") {
    auto ui = open(kBox, "<div id=\"x\"></div>");
    ui->ctx->setVerifyStaticSkip(true);  // Counts instead of aborting (the suites' env switch aborts).
    CHECK(ui->ctx->verifyStaticSkip());
    ui->frame();
    for (int i = 0; i < 2; ++i) {
        const FrameStats f = ui->frame();
        CHECK(f.paintSkipped == 1);  // Met the conditions...
        CHECK(f.paintMs > 0.0f);     // ...and was painted anyway.
        CHECK(f.staticSkipMismatch == 0);
    }
    // The test-only back door: a write behind the cause bookkeeping's back.
    const NodeId x = ui->el("x").node;
    ui->doc().arena().element(x).style.base[prop::BackgroundColor] = Value::ofColor(Color{1, 0, 0, 1});
    const FrameStats f = ui->frame();
    CHECK(f.staticSkipMismatch == 1);
    CHECK(dump(ui->list) == ui->fresh());  // The fresh paint is the output.
    CHECK(ui->ctx->visualCheck().mismatches == 1);
    CHECK(ui->ctx->visualCheck().firstNode == x);
    CHECK(ui->ctx->visualCheck().firstDocument == ui->handles[0]);
    CHECK(ui->ctx->visualCheck().firstKind >= 0);
    CHECK(ui->ctx->visualCheck().first.find("#x") != std::string::npos);
    CHECK(ui->frame().staticSkipMismatch == 0);  // The same (wrong) state again: nothing new.
}

TEST_CASE("static skip: one static and one animated document -- only the static one is skipped") {
    const std::vector<uint8_t> staticIr = compileOk(kBox, "<div id=\"x\">static</div>");
    const std::vector<uint8_t> movingIr = compileOk(std::string(kBox) +
                                                        "@keyframes pulse { from { opacity: 0.2 } to { "
                                                        "opacity: 1 } } #x { animation: pulse 1s infinite; }",
                                                    "<div id=\"x\"></div>");
    // Two Contexts, the second never skipping; same writes, same frames.
    std::unique_ptr<Ui> uis[2];
    for (int k = 0; k < 2; ++k) {
        uis[k] = std::make_unique<Ui>();
        Ui& ui = *uis[k];
        ui.irs = {staticIr, movingIr};
        ui.ctx = std::make_unique<Context>(ContextConfig{nullptr, &ui.assets});
        ui.ctx->setVerifyStaticSkip(false);
        ui.ctx->setStaticSkipEnabled(k == 0);
        ui.ctx->perf().setEnabled(true);
        ui.ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
        for (const auto& ir : ui.irs) {
            Result<DocumentHandle> h = ui.ctx->loadDocument(IrBlob{ir.data(), ir.size()}, false);
            REQUIRE(h.ok());
            ui.handles.push_back(h.value());
        }
    }
    for (int i = 0; i < 8; ++i) {
        const FrameStats a = uis[0]->frame();
        const FrameStats b = uis[1]->frame();
        CAPTURE(i);
        CHECK(a.paintSkipped == (i == 0 ? 0u : 1u));
        CHECK(b.paintSkipped == 0);
        CHECK(dump(uis[0]->list) == dump(uis[1]->list));
        CHECK(uis[0]->damage.full == uis[1]->damage.full);
        CHECK(uis[0]->damage.rects.size() == uis[1]->damage.rects.size());
    }
    // The static one changes once: both paint it; then it is skipped again.
    for (int k = 0; k < 2; ++k) {
        REQUIRE(uis[k]->el("x", 0).setStyle("width: 90px") == Status::kOk);
    }
    CHECK(uis[0]->frame().paintSkipped == 0);
    uis[1]->frame();
    CHECK(dump(uis[0]->list) == dump(uis[1]->list));
    CHECK(uis[0]->frame().paintSkipped == 1);
    uis[1]->frame();
    CHECK(dump(uis[0]->list) == dump(uis[1]->list));
}

TEST_CASE("static skip: xt_tests runs every Context in the verify mode with the skip enabled") {
    Context ctx{ContextConfig{}};
    CHECK(ctx.verifyStaticSkip());
    CHECK(ctx.staticSkipEnabled());
}

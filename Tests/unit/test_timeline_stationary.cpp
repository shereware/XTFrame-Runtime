#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-783 (Docs/tasks/T-783.md; 10 §10.16 W4, 23 §23.ag, 58 §58.3): a timeline group standing still
/// anywhere -- paused mid-way by its own marker, held after a host seekTimeline(), or stepped with no
/// time passing -- evaluates its members once and then leaves them alone (no track evaluation, no
/// writes, the visual generation does not move, so the static frame skips its paint) until it moves
/// again or something writes or restyles one of its targets. Every case runs in the static-skip
/// verify mode (xt_tests' default), which reports a skipped paint that would have shown something else.

using namespace xt;
using namespace xt::compile;

namespace {

/// `#A` opacity 0 -> 1 and translateX 0 -> 40 px linearly over the timeline `x` (2 s): opacity = t / 2.
std::string page(std::string_view markers, std::string_view triggers, std::string_view extraStyle = "",
                 std::string_view lastKey = "{ opacity: 1; transform: translateX(40px) }") {
    return "<xt><head><style>"
           "#A, #B { position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; background: #f00; }"
           "#B { left: 50px; }" +
           std::string(extraStyle) + "@timeline x { duration: 2s; marker: " + std::string(markers) +
           "; #A { 0s { opacity: 0; transform: translateX(0px) } 2s " + std::string(lastKey) +
           " } }</style></head><body id=\"Root\"><div id=\"A\" " + std::string(triggers) +
           "></div><div id=\"B\" port:go=\"number\" port:halt=\"number\" on:port(go)=\"resume(x)\"></div>"
           "</body></xt>";
}

std::vector<uint8_t> compileOk(const std::string& text) {
    CompileOptions options;
    options.emitMeta = false;
    return test::compileMainOk(text, options);
}

struct Ui {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    std::vector<uint8_t> reloaded;
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    float opacity(std::string_view id = "A") { return doc().byId(id).property(prop::Opacity).f; }
    float translateX(std::string_view id = "A") { return doc().byId(id).property(prop::TranslateX).f; }
    uint64_t generation() { return doc().visualGeneration(); }
    FrameStats frame(float dt = 1.0f / 40.0f) {
        ctx->update(dt);
        ctx->render(list, &damage);
        const FrameStats f = ctx->perf().current();
        CHECK(f.staticSkipMismatch == 0);
        return f;
    }
    void setPort(std::string_view name, float v) {
        const PortId id = doc().lookupPort(name);
        REQUIRE(id != 0);
        REQUIRE(doc().setPort(id, Value::ofFloat(v)) == Status::kOk);
    }
};

std::unique_ptr<Ui> open(const std::string& text) {
    auto ui = std::make_unique<Ui>();
    ui->ir = compileOk(text);
    ContextConfig config;
    config.assets = &ui->assets;
    ui->ctx = std::make_unique<Context>(config);
    ui->ctx->setVerifyStaticSkip(true);
    ui->ctx->perf().setEnabled(true);
    ui->ctx->setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> handle = ui->ctx->loadDocument(IrBlob{ui->ir.data(), ui->ir.size()}, false);
    REQUIRE(handle.ok());
    ui->handle = handle.value();
    for (int i = 0; i < 3; ++i) {
        ui->frame();
    }
    return ui;
}

/// Frames the group stands still: from the second one on nothing is evaluated, the paint is skipped
/// and the visual generation stays where it is.
void checkStill(Ui& ui, int frames = 5) {
    ui.frame();  // The frame it comes to rest (may still write once).
    const uint64_t generation = ui.generation();
    for (int i = 0; i < frames; ++i) {
        const FrameStats f = ui.frame();
        CAPTURE(i);
        CHECK(f.animEvaluated == 0);
        CHECK(f.paintSkipped == 1);
        CHECK(ui.generation() == generation);
    }
}

}  // namespace

TEST_CASE("T-783: paused mid-way by its own marker: nothing evaluated, the paint skipped; resume plays on") {
    auto ui = open(page("m 0.1s", "on:timeline(x).marker(m)=\"pause(x)\""));
    Document& d = ui->doc();
    REQUIRE(d.playTimeline("", "x") == Status::kOk);
    for (int i = 0; i < 6; ++i) {  // 0.15 s at 1/40 s: the marker is passed at 0.1.
        ui->frame();
    }
    REQUIRE_FALSE(d.isTimelinePlaying("", "x"));
    CHECK(d.timelineTime("", "x") == 0.1f);
    checkStill(*ui);
    CHECK(ui->opacity() == doctest::Approx(0.05f));
    CHECK(ui->translateX() == doctest::Approx(2.0f));
    ui->setPort("go", 1.0f);  // resume(x)
    ui->frame();
    ui->frame();
    CHECK(d.isTimelinePlaying("", "x"));
    const FrameStats g = ui->frame();
    CHECK(g.animEvaluated == 1);
    CHECK(g.paintSkipped == 0);
    CHECK(d.timelineTime("", "x") > 0.1f);
    CHECK(ui->opacity() > 0.05f);
}

TEST_CASE("T-783: held after a host seekTimeline(): still; a write or restyle of the target re-applies it") {
    auto ui = open(page("m 1.5s", "", ".faded { opacity: 0.3; transform: translateX(5px); }"));
    Document& d = ui->doc();
    REQUIRE(d.seekTimeline("", "x", 0.5f) == Status::kOk);
    checkStill(*ui);
    CHECK(ui->opacity() == doctest::Approx(0.25f));
    CHECK(ui->translateX() == doctest::Approx(10.0f));

    SUBCASE("a host write before the frame: evaluated once, the timeline's value on top") {
        d.byId("A").setProperty(prop::Opacity, Value::ofFloat(0.9f));
        const FrameStats f = ui->frame();
        CHECK(f.animEvaluated == 1);
        CHECK(ui->opacity() == doctest::Approx(0.25f));
        checkStill(*ui);
        CHECK(ui->opacity() == doctest::Approx(0.25f));
    }
    SUBCASE("a host write between update and render: shown that frame, the timeline's value the next") {
        ui->ctx->update(1.0f / 40.0f);
        d.byId("A").setProperty(prop::Opacity, Value::ofFloat(0.9f));
        ui->ctx->render(ui->list, &ui->damage);
        CHECK(ui->ctx->perf().current().paintSkipped == 0);
        CHECK(ui->ctx->perf().current().staticSkipMismatch == 0);
        const FrameStats f = ui->frame();
        CHECK(f.animEvaluated == 1);
        CHECK(f.paintSkipped == 0);
        CHECK(ui->opacity() == doctest::Approx(0.25f));
        checkStill(*ui);
    }
    SUBCASE("a class change restyles the target: the held values stay on top") {
        REQUIRE(d.byId("A").addClass("faded"));
        CHECK(ui->frame().animEvaluated == 1);
        CHECK(ui->opacity() == doctest::Approx(0.25f));
        CHECK(ui->translateX() == doctest::Approx(10.0f));
        checkStill(*ui);
        REQUIRE(d.byId("A").removeClass("faded"));
        ui->frame();
        CHECK(ui->opacity() == doctest::Approx(0.25f));
        CHECK(ui->translateX() == doctest::Approx(10.0f));
        checkStill(*ui);
    }
    SUBCASE("a seek to another time evaluates once, the same time not at all") {
        REQUIRE(d.seekTimeline("", "x", 1.0f) == Status::kOk);
        CHECK(ui->frame().animEvaluated == 1);
        CHECK(ui->opacity() == doctest::Approx(0.5f));
        checkStill(*ui);
        REQUIRE(d.seekTimeline("", "x", 1.0f) == Status::kOk);
        const FrameStats f = ui->frame();
        CHECK(f.animEvaluated == 0);
        CHECK(f.paintSkipped == 1);
    }
    SUBCASE("played again: every frame evaluates") {
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        for (int i = 0; i < 4; ++i) {
            const FrameStats f = ui->frame();
            CHECK(f.animEvaluated == 1);
            CHECK(f.paintSkipped == 0);
        }
    }
}

TEST_CASE("T-783: a held member still wins over the element's own animation and a transition") {
    auto ui = open(page("m 1.5s", "",
                        "@keyframes dim { from { opacity: 0.6 } to { opacity: 0.6 } }"
                        "@keyframes glow { from { background-color: #f00 } to { background-color: #00f } }"
                        ".dim { animation: dim 10s linear; } .glow { animation: glow 10s linear infinite; }"
                        "#A { transition: opacity 0.2s linear; } .half { opacity: 0.5; }"));
    Document& d = ui->doc();
    REQUIRE(d.seekTimeline("", "x", 0.5f) == Status::kOk);
    checkStill(*ui);
    SUBCASE("an animation of the same property: the member writes over it every frame") {
        REQUIRE(d.byId("A").addClass("dim"));
        for (int i = 0; i < 6; ++i) {
            ui->frame();
            CHECK(ui->opacity() == doctest::Approx(0.25f));
        }
    }
    SUBCASE("an animation of another property does not wake the member") {
        REQUIRE(d.byId("A").addClass("glow"));
        ui->frame();
        ui->frame();
        for (int i = 0; i < 4; ++i) {
            CHECK(ui->frame().animEvaluated == 1);  // glow only.
            CHECK(ui->opacity() == doctest::Approx(0.25f));
        }
    }
    SUBCASE("a transition of the property: the member's value stays on top") {
        REQUIRE(d.byId("A").addClass("half"));
        for (int i = 0; i < 12; ++i) {
            ui->frame();
            CHECK(ui->opacity() == doctest::Approx(0.25f));
        }
        checkStill(*ui);
    }
}

TEST_CASE("T-783: a hot reload of the timeline's definition while it stands still evaluates it again") {
    auto ui = open(page("m 1.5s", ""));
    REQUIRE(ui->doc().seekTimeline("", "x", 0.5f) == Status::kOk);
    checkStill(*ui);
    CHECK(ui->opacity() == doctest::Approx(0.25f));
    ui->reloaded = compileOk(page("m 1.5s", "", "", "{ opacity: 0.5; transform: translateX(80px) }"));
    BundleDelta delta;
    delta.ir.push_back(IrReload{ui->handle, IrBlob{ui->reloaded.data(), ui->reloaded.size()}, false});
    REQUIRE(ui->ctx->hotReload(delta) == Status::kOk);
    const FrameStats f = ui->frame();
    CHECK(f.animEvaluated == 1);
    CHECK(f.paintSkipped == 0);
    CHECK(ui->doc().timelineTime("", "x") == doctest::Approx(0.5f));
    CHECK(ui->opacity() == doctest::Approx(0.125f));
    CHECK(ui->translateX() == doctest::Approx(20.0f));
    checkStill(*ui);
}

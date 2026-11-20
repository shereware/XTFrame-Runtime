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

/// T-749 (Docs/design/23-host-integration.md §23.ag item 6): Context::needsFrame(). False only when
/// update() at any later clock changes nothing and render() hands out the previous list again (every
/// paint skipped, no damage); any write, input or host call that changes the UI makes it true; whatever
/// moves on with time alone (animations, transitions in their delay, timelines, the caret, particles, a
/// held gamepad direction) keeps it true until it stops. The suite's verify mode (every Context checks
/// its skipped paints against a fresh one and aborts on a difference) stays on here.

using namespace xt;
using namespace xt::compile;

namespace {

std::vector<uint8_t> compileNeeds(std::string_view style, std::string_view body) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string text = "<xt><head><style>" + std::string(style) +
                             "</style></head>\n<body id=\"Root\">" + std::string(body) + "</body></xt>";
    return test::compileMainOk(text, options);
}

struct Host {
    FileAssetResolver assets{std::string(XT_TEST_XT_DIR), nullptr};
    std::unique_ptr<Context> ctx;
    std::vector<uint8_t> ir;
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    ElementRef el(std::string_view id) { return doc().byId(id); }
    /// One frame as a host draws it (damage asked for, the previous list handed back).
    FrameStats frame(float dt = 1.0f / 60.0f) {
        ctx->update(dt);
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
    /// Frames until needsFrame() is false, at most `limit`; how many it took (0: already false), -1 never.
    int settle(int limit = 12, float dt = 1.0f / 60.0f) {
        for (int i = 0; i <= limit; ++i) {
            if (!ctx->needsFrame()) {
                return i;
            }
            frame(dt);
        }
        return -1;
    }
    /// needsFrame() is false and means it: an update at a much later clock moves no generation and
    /// keeps it false, and the render after it skips the paint with no damage and the same commands.
    void checkIdle() {
        REQUIRE_FALSE(ctx->needsFrame());
        const uint64_t generation = doc().visualGeneration();
        std::string before;
        dumpCommandList(list, before);
        ctx->update(2.5f);
        CHECK(doc().visualGeneration() == generation);
        CHECK_FALSE(ctx->needsFrame());
        ctx->render(list, &damage);
        CHECK(ctx->perf().current().paintSkipped == (ctx->isVisible(handle) ? 1u : 0u));
        CHECK(ctx->perf().current().staticSkipMismatch == 0);
        CHECK_FALSE(damage.full);
        CHECK(damage.rects.empty());
        std::string after;
        dumpCommandList(list, after);
        CHECK(after == before);
        CHECK_FALSE(ctx->needsFrame());
    }
};

std::unique_ptr<Host> openNeeds(std::string_view style, std::string_view body) {
    auto host = std::make_unique<Host>();
    host->ir = compileNeeds(style, body);
    host->ctx = std::make_unique<Context>(ContextConfig{nullptr, &host->assets});
    host->ctx->perf().setEnabled(true);
    host->ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
    Result<DocumentHandle> handle = host->ctx->loadDocument(IrBlob{host->ir.data(), host->ir.size()}, false);
    REQUIRE(handle.ok());
    host->handle = handle.value();
    return host;
}

constexpr const char* kBoxStyle = "#x { width: 40px; height: 20px; background-color: #336699; }";

/// `write` (between frames) makes needsFrame() true; frames bring it back to false within `within`.
void checkWakes(Host& host, const std::function<void()>& write, int within = 3) {
    REQUIRE(host.settle() >= 0);
    host.checkIdle();
    write();
    CHECK(host.ctx->needsFrame());
    const int took = host.settle();
    CHECK(took >= 1);
    CHECK(took <= within);
    host.checkIdle();
}

}  // namespace

TEST_CASE("needsFrame: a static document is idle after its first frame") {
    auto host = openNeeds(kBoxStyle, "<div id=\"x\">static</div>");
    CHECK(host->ctx->needsFrame());  // Nothing updated or rendered yet.
    host->ctx->update(0.0f);
    CHECK(host->ctx->needsFrame());  // Updated, not rendered.
    host->ctx->render(host->list, &host->damage);
    host->checkIdle();
    for (int i = 0; i < 3; ++i) {
        host->checkIdle();
    }
}

TEST_CASE("needsFrame: every kind of write and input wakes it; a frame or two settles it again") {
    SUBCASE("setStyle / setText / custom state / class") {
        auto host = openNeeds(std::string(kBoxStyle) +
                                  "#x:state(Lit) { background-color: #ff0000; }"
                                  ".wide { width: 90px; }",
                              "<div id=\"x\">a</div>");
        checkWakes(*host, [&] { REQUIRE(host->el("x").setStyle("width: 60px") == Status::kOk); });
        checkWakes(*host, [&] { host->el("x").setText("b"); });
        checkWakes(*host,
                   [&] { REQUIRE(host->doc().setCustomState(host->el("x"), "Lit", true) == Status::kOk); });
        checkWakes(*host, [&] { REQUIRE(host->el("x").addClass("wide")); });
    }
    SUBCASE("a port through its binding") {
        auto host = openNeeds(kBoxStyle, "<div id=\"x\" port:hp=\"number\" bind:width=\"hp\"></div>");
        checkWakes(*host, [&] {
            REQUIRE(host->doc().setPort(host->doc().lookupPort("hp"), Value::ofFloat(77)) == Status::kOk);
        });
    }
    SUBCASE("a sticky override") {
        auto host = openNeeds(kBoxStyle, "<div id=\"x\"></div>");
        checkWakes(*host,
                   [&] { host->el("x").setProperty(prop::Opacity, Value::ofFloat(0.5f), /*sticky=*/true); });
    }
    SUBCASE("pointer input (hover)") {
        auto host = openNeeds(std::string(kBoxStyle) + "#x:hover { background-color: #ff0000; }",
                              "<div id=\"x\"></div>");
        checkWakes(*host, [&] {
            InputEvent move;
            move.kind = InputEvent::PointerMove;
            move.pos = Vec2{10, 10};
            host->ctx->pushInput(move);
            CHECK(host->ctx->pendingInputCount() == 1);
        });
        // A move that changes nothing visible still wakes the frame that dispatches it.
        checkWakes(*host, [&] {
            InputEvent move;
            move.kind = InputEvent::PointerMove;
            move.pos = Vec2{12, 11};
            host->ctx->pushInput(move);
        });
    }
    SUBCASE("viewport, token, theme and paint options") {
        auto host = openNeeds("#x { width: 40px; height: 20px; background-color: var(--c, #336699); }",
                              "<div id=\"x\"></div>");
        checkWakes(*host, [&] { host->ctx->setViewport(Rect{0, 0, 500, 300}, 1.0f); });
        checkWakes(*host, [&] { host->ctx->setViewport(Rect{0, 0, 500, 300}, 2.0f); });
        checkWakes(*host, [&] {
            REQUIRE(host->ctx->setToken("--c", Value::ofColor(Color{1, 0, 0, 1})) == Status::kOk);
        });
        checkWakes(*host, [&] { host->ctx->setBoxShadowCommands(false); });
        checkWakes(*host, [&] { host->ctx->setHostTargetMode(HostTargetMode::Direct); });
    }
    SUBCASE("hide and show") {
        auto host = openNeeds(kBoxStyle, "<div id=\"x\"></div>");
        checkWakes(*host, [&] { REQUIRE(host->ctx->requestHide(host->handle) == Status::kOk); });
        CHECK_FALSE(host->ctx->isVisible(host->handle));
        checkWakes(*host, [&] { REQUIRE(host->ctx->requestShow(host->handle) == Status::kOk); });
    }
    SUBCASE("a reload") {
        auto host = openNeeds(kBoxStyle, "<div id=\"x\"></div>");
        checkWakes(*host, [&] {
            REQUIRE(host->ctx->reloadDocument(host->handle, IrBlob{host->ir.data(), host->ir.size()}, false)
                        .ok());
        });
    }
}

TEST_CASE("needsFrame: a write between update and render is seen after the render") {
    auto host = openNeeds(kBoxStyle, "<div id=\"x\">a</div>");
    REQUIRE(host->settle() >= 0);
    host->ctx->update(1.0f / 60.0f);
    REQUIRE(host->el("x").setStyle("width: 70px") == Status::kOk);
    host->ctx->render(host->list, &host->damage);
    CHECK(host->ctx->needsFrame());  // The restyle is resolved by the next update.
    CHECK(host->settle() >= 1);
    host->checkIdle();
}

TEST_CASE("needsFrame: what moves with time keeps it true until it stops") {
    SUBCASE("an infinite animation never settles") {
        auto host = openNeeds(std::string(kBoxStyle) +
                                  "@keyframes pulse { from { opacity: 0.2 } to { opacity: 1 } }"
                                  "#x { animation: pulse 1s infinite; }",
                              "<div id=\"x\"></div>");
        for (int i = 0; i < 20; ++i) {
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
    }
    SUBCASE("a finite animation holding at forwards: true while it plays, then idle") {
        auto host = openNeeds(std::string(kBoxStyle) +
                                  "@keyframes fade { from { opacity: 1 } to { opacity: 0.25 } }"
                                  "#x { animation: fade 0.25s linear forwards; }",
                              "<div id=\"x\"></div>");
        for (int i = 0; i < 12; ++i) {  // 0.2 s of the 0.25 s
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
        const int took = host->settle(20);
        CHECK(took > 0);
        CHECK(took <= 8);
        host->checkIdle();
        CHECK(getProperty(host->doc().arena(), host->el("x").node, prop::Opacity).f == 0.25f);
        // The held instance crosses no more iterations (it queued an AnimationIteration per 0.25 s).
        int iterations = 0;
        host->doc().addListener(host->el("x"), EventType::AnimationIteration, [&](Event&) { ++iterations; });
        for (int i = 0; i < 4; ++i) {
            host->frame(1.0f);
        }
        CHECK(iterations == 0);
        CHECK_FALSE(host->ctx->needsFrame());
    }
    SUBCASE("a transition in its delay changes nothing yet but keeps it true") {
        auto host =
            openNeeds(std::string(kBoxStyle) +
                          "#x { opacity: 1; transition: opacity 0.1s linear 0.5s; } #x.dim { opacity: 0.2; }",
                      "<div id=\"x\"></div>");
        REQUIRE(host->settle() >= 0);
        REQUIRE(host->el("x").addClass("dim"));
        for (int i = 0; i < 24; ++i) {  // 0.4 s: still in the delay
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
        CHECK(host->settle(30) > 0);
        host->checkIdle();
        CHECK(getProperty(host->doc().arena(), host->el("x").node, prop::Opacity).f == 0.2f);
    }
    SUBCASE("a timeline group: true while it plays, idle once held at its end") {
        auto host = openNeeds(
            std::string(kBoxStyle) + "@timeline intro { #x { 0s { opacity: 0 } 0.2s { opacity: 1 } } }",
            "<div id=\"x\"></div>");
        REQUIRE(host->settle() >= 0);
        REQUIRE(host->ctx->playTimeline("", "intro") == Status::kOk);
        CHECK(host->ctx->needsFrame());
        for (int i = 0; i < 8; ++i) {
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
        CHECK(host->settle(30) > 0);
        host->checkIdle();
        // A seek of the held group wakes it once more.
        checkWakes(*host, [&] { REQUIRE(host->ctx->seekTimeline("", "intro", 0.1f) == Status::kOk); });
    }
    SUBCASE("a focused input's caret") {
        auto host =
            openNeeds("#i { width: 100px; height: 20px; }", "<input id=\"i\" type=\"text\" value=\"abc\"/>");
        REQUIRE(host->settle() >= 0);
        host->doc().setFocus(host->el("i"));
        for (int i = 0; i < 6; ++i) {
            host->frame(0.3f);
            CHECK(host->ctx->needsFrame());
        }
        host->doc().setFocus(ElementRef{});  // Blur: no caret, idle again.
        CHECK(host->settle() > 0);
        host->checkIdle();
    }
    SUBCASE("particles") {
        auto host =
            openNeeds("", R"~(<particles burst="6" rate="0" lifetime="0.2" size="4" color="#ffffff80" autoplay
                                        style="position: absolute; left: 20px; top: 30px; width: 40px; height: 40px">
                                      </particles>)~");
        for (int i = 0; i < 6; ++i) {  // 0.1 s: the burst is alive
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
        CHECK(host->settle(40) > 0);  // Every particle retired (no rate): nothing moves any more.
        host->checkIdle();
    }
    SUBCASE("a held d-pad direction repeats") {
        auto host =
            openNeeds("button { width: 40px; height: 20px; }",
                      "<button id=\"a\">A</button><button id=\"b\">B</button><button id=\"c\">C</button>");
        REQUIRE(host->settle() >= 0);
        InputEvent down;
        down.kind = InputEvent::GamepadButtonDown;
        down.gamepadButton = GamepadButton::DpadDown;
        host->ctx->pushInput(down);
        for (int i = 0; i < 30; ++i) {
            host->frame();
            CHECK(host->ctx->needsFrame());
        }
        InputEvent up = down;
        up.kind = InputEvent::GamepadButtonUp;
        host->ctx->pushInput(up);
        CHECK(host->settle() > 0);
        host->checkIdle();
    }
}

TEST_CASE("needsFrame: an idle host that skips frames shows what one that never skips shows") {
    // Two Contexts on the same document and the same writes at the same frames; the first skips its
    // update and render whenever needsFrame() is false, passing the time it skipped with the next update.
    const std::vector<uint8_t> ir = compileNeeds(
        std::string(kBoxStyle) +
            "#x { transition: opacity 0.2s linear; } #x.dim { opacity: 0.3; }"
            "@keyframes nudge { from { width: 40px } to { width: 60px } } #x.go { animation: nudge 0.3s; }",
        "<div id=\"x\">a</div>");
    Host hosts[2];
    for (Host& h : hosts) {
        h.ctx = std::make_unique<Context>(ContextConfig{nullptr, &h.assets});
        h.ctx->perf().setEnabled(true);
        h.ctx->setViewport(Rect{0, 0, 400, 300}, 1.0f);
        Result<DocumentHandle> handle = h.ctx->loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(handle.ok());
        h.handle = handle.value();
    }
    const float dt = 0.25f;  // Exact in binary: the skipped time adds up to the same clock.
    float skipped = 0.0f;
    int skips = 0;
    for (int frame = 0; frame < 40; ++frame) {
        for (Host& h : hosts) {
            if (frame == 10) {
                REQUIRE(h.el("x").addClass("dim"));
            }
            if (frame == 20) {
                REQUIRE(h.el("x").addClass("go"));
            }
            if (frame == 30) {
                h.el("x").setText("b");
            }
        }
        hosts[1].frame(dt);
        if (hosts[0].ctx->needsFrame()) {
            hosts[0].frame(dt + skipped);
            skipped = 0.0f;
        } else {
            skipped += dt;
            ++skips;
        }
        // A skipped frame's picture is the previous one, which must still be the current one.
        std::string a, b;
        dumpCommandList(hosts[0].list, a);
        dumpCommandList(hosts[1].list, b);
        CAPTURE(frame);
        CHECK(a == b);
    }
    CHECK(skips > 20);
    CHECK(hosts[0].ctx->clock() + skipped == hosts[1].ctx->clock());
}

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/runtime_state.h"
#include "xt/types.h"

/// T-616 (Docs/design/04-timeline.md §4.13): a CSS `animation` item keeps its Animator instance
/// across restyles and parameter changes (identity = position + `@keyframes`), and
/// Document::captureRuntimeState() / restoreRuntimeState() carry the running state over to a
/// recompiled Document.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;

    void stepTo(float t) {
        clock = t;
        doc->update(clock);
    }
    std::vector<uint32_t> anims(std::string_view id) const { return doc->animationsOf(doc->byId(id)); }
    float opacity(std::string_view id) const { return doc->byId(id).property(prop::Opacity).f; }
};

/// Compiled and loaded, before its first update().
Ui load(const std::string& xt) {
    Ui out;
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    out.result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    return out;
}

Ui open(const std::string& xt) {
    Ui out = load(xt);
    out.doc->update(0.0f);
    return out;
}

constexpr const char* kFade = "@keyframes fade { from { opacity: 1 } to { opacity: 0 } }";
constexpr const char* kKeyframes =
    "@keyframes a { from { opacity: 1 } to { opacity: 0 } }"
    "@keyframes b { from { width: 0px } to { width: 100px } }"
    "@keyframes c { from { height: 0px } to { height: 100px } }";

}  // namespace

TEST_CASE("T-616: a restyle does not restart a CSS animation") {
    Ui ui =
        open(page(std::string(kFade) + "#x { width: 50px; height: 20px; animation: fade 2s linear infinite; }"
                                       ".alt { animation: fade 2s linear infinite; color: red; }",
                  R"X(<div id="x">x</div>)X"));
    ui.stepTo(0.5f);
    const std::vector<uint32_t> before = ui.anims("x");
    REQUIRE(before.size() == 1);
    CHECK(ui.opacity("x") == doctest::Approx(0.75f));

    ElementRef x = ui.doc->byId("x");
    REQUIRE(x.setStyle("color: #ff0000") == Status::kOk);
    REQUIRE(x.setStyle("width: 80px") == Status::kOk);
    ui.stepTo(0.6f);
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.7f));

    // Another rule wins with the same `animation`: the same item, not a new one.
    x.addClass("alt");
    ui.stepTo(1.0f);
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.5f));
    // A runtime declaration with a changed item takes over without a restart too.
    REQUIRE(x.setStyle("animation: fade 2s ease-in infinite") == Status::kOk);
    ui.stepTo(1.2f);
    CHECK(ui.anims("x") == before);
    REQUIRE(x.setStyle("animation: fade 2s linear infinite") == Status::kOk);
    ui.stepTo(1.2f);
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.4f));
    // Declaring exactly the playing list again (xt_flow's `playAnimation`, 17.6) restarts it.
    REQUIRE(x.setStyle("animation: fade 2s linear infinite") == Status::kOk);
    ui.stepTo(1.3f);
    REQUIRE(ui.anims("x").size() == 1);
    CHECK(ui.anims("x")[0] != before[0]);
    CHECK(ui.opacity("x") == doctest::Approx(1.0f));
}

TEST_CASE("T-616: a new animation-duration continues from the elapsed seconds") {
    Ui ui = open(
        page(std::string(kFade) + "#x { width: 50px; height: 20px; animation: fade 2s linear infinite; }",
             R"X(<div id="x">x</div>)X"));
    ui.stepTo(1.0f);
    const std::vector<uint32_t> before = ui.anims("x");
    REQUIRE(before.size() == 1);
    CHECK(ui.opacity("x") == doctest::Approx(0.5f));

    ElementRef x = ui.doc->byId("x");
    REQUIRE(x.setStyle("animation: fade 4s linear infinite") == Status::kOk);
    ui.stepTo(1.0f);
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.75f));  // 1 s of 4, not half of 4.
    ui.stepTo(2.0f);
    CHECK(ui.opacity("x") == doctest::Approx(0.5f));

    // Shorter than what has elapsed, one iteration, forwards: it ends at its last frame.
    REQUIRE(x.setStyle("animation: fade 1s linear forwards") == Status::kOk);
    ui.stepTo(2.1f);
    CHECK(ui.anims("x") == before);
    CHECK_FALSE(ui.doc->isPlaying(before[0]));
    CHECK(ui.opacity("x") == doctest::Approx(0.0f));
}

TEST_CASE("T-616: a removed name stops, an added one plays, a reordered list keeps every instance") {
    Ui ui = open(page(std::string(kKeyframes) + "#x { position: absolute; }", R"X(<div id="x">x</div>)X"));
    ElementRef x = ui.doc->byId("x");
    REQUIRE(x.setStyle("animation: a 1s linear infinite, b 1s linear infinite") == Status::kOk);
    ui.stepTo(0.1f);
    const std::vector<uint32_t> ab = ui.anims("x");
    REQUIRE(ab.size() == 2);

    REQUIRE(x.setStyle("animation: b 1s linear infinite, a 1s linear infinite") == Status::kOk);
    ui.stepTo(0.2f);
    CHECK(ui.anims("x") == std::vector<uint32_t>{ab[1], ab[0]});
    CHECK(ui.doc->isPlaying(ab[0]));
    CHECK(ui.doc->isPlaying(ab[1]));

    REQUIRE(x.setStyle("animation: a 1s linear infinite") == Status::kOk);
    ui.stepTo(0.3f);
    CHECK(ui.anims("x") == std::vector<uint32_t>{ab[0]});
    CHECK_FALSE(ui.doc->isPlaying(ab[1]));
    CHECK(ui.opacity("x") == doctest::Approx(0.8f));  // `a` from 0.1, never restarted.

    REQUIRE(x.setStyle("animation: a 1s linear infinite, c 1s linear infinite") == Status::kOk);
    ui.stepTo(0.4f);
    const std::vector<uint32_t> ac = ui.anims("x");
    REQUIRE(ac.size() == 2);
    CHECK(ac[0] == ab[0]);
    CHECK(ac[1] != ab[0]);
    CHECK(ac[1] != ab[1]);
    CHECK(ui.doc->isPlaying(ac[1]));
    CHECK(ui.opacity("x") == doctest::Approx(0.7f));

    // The same name twice: the second is its own item (position), dropped alone.
    REQUIRE(x.setStyle("animation: a 1s linear infinite, a 2s linear infinite") == Status::kOk);
    ui.stepTo(0.5f);
    const std::vector<uint32_t> aa = ui.anims("x");
    REQUIRE(aa.size() == 2);
    CHECK(aa[0] == ab[0]);
    REQUIRE(x.setStyle("animation: a 1s linear infinite") == Status::kOk);
    ui.stepTo(0.6f);
    CHECK(ui.anims("x") == std::vector<uint32_t>{ab[0]});
    CHECK_FALSE(ui.doc->isPlaying(aa[1]));
}

TEST_CASE("T-616: animation-play-state pauses and resumes the same instance") {
    Ui ui = open(page(std::string(kFade) + "#x { width: 50px; height: 20px; }", R"X(<div id="x">x</div>)X"));
    ElementRef x = ui.doc->byId("x");
    REQUIRE(x.setStyle("animation: fade 2s linear infinite") == Status::kOk);
    ui.stepTo(0.0f);
    const std::vector<uint32_t> before = ui.anims("x");
    REQUIRE(before.size() == 1);
    ui.stepTo(0.5f);
    REQUIRE(x.setStyle("animation: fade 2s linear infinite paused") == Status::kOk);
    ui.stepTo(0.5f);
    ui.stepTo(1.5f);
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.75f));
    REQUIRE(x.setStyle("animation: fade 2s linear infinite running") == Status::kOk);
    ui.stepTo(2.0f);  // Resumed here, at 0.5 s.
    CHECK(ui.anims("x") == before);
    CHECK(ui.opacity("x") == doctest::Approx(0.75f));
    ui.stepTo(2.5f);
    CHECK(ui.opacity("x") == doctest::Approx(0.5f));
}

TEST_CASE(
    "T-616: restoreRuntimeState carries CSS animations to a recompiled document, @keyframes edits in place") {
    Ui a = open(page(std::string(kFade) +
                         "#x { width: 50px; height: 20px; animation: fade 2s linear infinite; }"
                         "#y { width: 50px; height: 20px; animation: fade 1s linear forwards; }"
                         "#z { width: 50px; height: 20px; animation: fade 4s linear infinite; }",
                     R"X(<div id="x">x</div><div id="y">y</div><div id="z">z</div>)X"));
    a.stepTo(1.5f);
    a.doc->pause(a.anims("z")[0]);  // As the editor's playhead holds it.
    a.stepTo(2.5f);
    CHECK(a.opacity("x") == doctest::Approx(0.75f));  // 2.5 s into a 2 s loop.
    CHECK(a.opacity("y") == doctest::Approx(0.0f));
    CHECK(a.opacity("z") == doctest::Approx(0.625f));
    RuntimeState state = a.doc->captureRuntimeState();
    CHECK_FALSE(state.empty());
    CHECK(state.clock() == doctest::Approx(2.5f));

    // The recompiled source: `fade` now ends at 0.5 and #x got a color.
    Ui b =
        load(page("@keyframes fade { from { opacity: 1 } to { opacity: 0.5 } }"
                  "#x { width: 50px; height: 20px; color: red; animation: fade 2s linear infinite; }"
                  "#y { width: 50px; height: 20px; animation: fade 1s linear forwards; }"
                  "#z { width: 50px; height: 20px; animation: fade 4s linear infinite; }"
                  "#w { width: 50px; height: 20px; animation: fade 1s linear; }",
                  R"X(<div id="x">x</div><div id="y">y</div><div id="z">z</div><div id="w">w</div>)X"));
    b.doc->restoreRuntimeState(state);
    b.stepTo(2.5f);
    CHECK(b.opacity("x") == doctest::Approx(0.875f));   // Local 0.25 of the new keyframes.
    CHECK(b.opacity("y") == doctest::Approx(0.5f));     // Ended, held by its forwards fill.
    CHECK(b.opacity("z") == doctest::Approx(0.8125f));  // Paused at 1.5 s of 4.
    CHECK(b.doc->isPlaying(b.anims("w")[0]));           // #w is new: it starts.
    b.stepTo(2.6f);
    CHECK(b.opacity("x") == doctest::Approx(0.85f));
    CHECK(b.opacity("z") == doctest::Approx(0.8125f));
    CHECK(b.doc->animator().isPaused(b.anims("z")[0]));
}

TEST_CASE("T-616: an animation that ended and was dropped stays ended after restoreRuntimeState") {
    const std::string xt =
        page(std::string(kFade) + "#w { width: 50px; height: 20px; animation: fade 1s linear; }",
             R"X(<div id="w">w</div>)X");
    Ui a = open(xt);
    a.stepTo(0.5f);
    a.stepTo(1.5f);
    const uint32_t old = a.anims("w")[0];
    CHECK_FALSE(a.doc->isPlaying(old));
    RuntimeState state = a.doc->captureRuntimeState();

    Ui b = load(xt);
    b.doc->restoreRuntimeState(state);
    b.stepTo(1.6f);
    REQUIRE(b.anims("w").size() == 1);
    CHECK_FALSE(b.doc->isPlaying(b.anims("w")[0]));
    CHECK(b.opacity("w") == doctest::Approx(1.0f));  // No fill: the styled value.

    // An empty state restores nothing: the new document starts on its own.
    Ui c = load(xt);
    c.doc->restoreRuntimeState(RuntimeState{});
    c.stepTo(1.6f);
    REQUIRE(c.anims("w").size() == 1);
    CHECK(c.doc->isPlaying(c.anims("w")[0]));
}

TEST_CASE("T-616: restoreRuntimeState carries a playing @timeline group and custom states") {
    const std::string xt = page(
        "#g { position: absolute; height: 10px; width: 0px; }"
        "@timeline tl { #g { 0s { width: 0px } 2s { width: 100px } } }"
        "#g:state(Open) { height: 30px; }",
        R"X(<div id="g">g</div>)X");
    Ui a = open(xt);
    REQUIRE(a.doc->playTimeline("", "tl") == Status::kOk);
    REQUIRE(a.doc->setCustomState(a.doc->byId("g"), "Open", true) == Status::kOk);
    a.stepTo(0.5f);
    a.stepTo(1.0f);
    CHECK(a.doc->timelineTime("", "tl") == doctest::Approx(1.0f));  // Played at clock 0.
    RuntimeState state = a.doc->captureRuntimeState();

    Ui b = load(xt);
    b.doc->restoreRuntimeState(state);
    CHECK(b.doc->isTimelinePlaying("", "tl"));
    CHECK(b.doc->hasCustomState(b.doc->byId("g"), "Open"));
    b.stepTo(1.5f);
    CHECK(b.doc->timelineTime("", "tl") == doctest::Approx(1.5f));
    CHECK(b.doc->byId("g").property(prop::Width).len.value == doctest::Approx(75.0f));
}

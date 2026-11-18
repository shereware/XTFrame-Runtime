#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// T-717 (Docs/design/10-backends.md §10.16 W4): a `@timeline` group member paused at its end stop
/// (forward end, or the start after a reverse play) with a fill that keeps that value is evaluated
/// once and then left alone -- no track evaluation, no writes, not "in motion" -- until something
/// moves it off the stop or overwrites one of its target nodes.

using namespace xt;
using namespace xt::compile;

namespace {

constexpr const char* kSlideStyle = R"(
  #Box { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
  #Child { position: absolute; left: 1px; top: 1px; width: 10px; height: 10px; }
  .faded { opacity: 0.3; transform: translateX(5px); }
  @timeline slide {
    #Box { 0s { opacity: 0; transform: translateX(0px) } .5s { opacity: 1; transform: translateX(20px) } }
  }
  @timeline blink { #Child { 0s { opacity: 0 } .2s { opacity: 1 } } }
)";

std::string doc(std::string_view style,
                std::string_view body = "<div id=\"Box\"><div id=\"Child\"></div></div>") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
    PerfTracker perf;
};

std::unique_ptr<Loaded> load(const std::string& xt) {
    auto out = std::make_unique<Loaded>();
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;
    const std::vector<SourceFile> files{{"main.xt", xt}};
    out->result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : out->result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out->result.ok());
    Status status = Document::load(IrBlob{out->result.ir.data(), out->result.ir.size()}, false, nullptr,
                                   nullptr, out->doc);
    REQUIRE(status == Status::kOk);
    out->perf.setEnabled(true);
    out->doc->setPerfTracker(&out->perf);
    out->doc->setViewport(Rect{0, 0, 200, 100});
    out->perf.beginFrame(0.0f);
    out->doc->update(0.0f);
    return out;
}

/// One frame at `t`; returns that frame's FrameStats::animEvaluated.
uint32_t step(Loaded& l, float t) {
    l.perf.beginFrame(t);
    l.doc->update(t);
    return l.perf.current().animEvaluated;
}

float opacityOf(Document& d, std::string_view id) { return d.byId(id).property(prop::Opacity).f; }
float translateXOf(Document& d, std::string_view id) { return d.byId(id).property(prop::TranslateX).f; }
bool moving(Document& d, std::string_view id) { return inMotion(d.arena(), d.byId(id).node); }

PlayOptions holdOptions(bool reverse = false) {
    PlayOptions opts;
    opts.hold = true;
    opts.reverse = reverse;
    return opts;
}

/// Plays `slide` with hold to its end (the frame at 0.6 s ends it) and returns the next frame time.
float playToHeldEnd(Loaded& l, bool reverse = false) {
    REQUIRE(l.doc->playTimeline("", "slide", holdOptions(reverse)) == Status::kOk);
    CHECK(step(l, 0.25f) == 1);
    CHECK(moving(*l.doc, "Child"));
    CHECK(step(l, 0.6f) == 1);  // The frame reaching the stop writes the final values once.
    return 0.7f;
}

}  // namespace

TEST_CASE("T-717: a timeline played to its end and held stops evaluating; values stay, nothing moves") {
    auto l = load(doc(kSlideStyle));
    Document& d = *l->doc;
    float t = playToHeldEnd(*l);
    for (int i = 0; i < 10; ++i, t += 0.1f) {
        CHECK(step(*l, t) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(20.0f));
        CHECK_FALSE(moving(d, "Box"));
        CHECK_FALSE(moving(d, "Child"));
    }
    // The pool still holds the member (activeAnimations is the pool size, unchanged).
    CHECK(l->perf.current().activeAnimations == 1);
    CHECK(d.isTimelinePlaying("", "slide") == false);
}

TEST_CASE("T-717: reverse-played to the start and held stops evaluating too") {
    auto l = load(doc(kSlideStyle));
    Document& d = *l->doc;
    float t = playToHeldEnd(*l, /*reverse=*/true);
    CHECK(d.timelineTime("", "slide") == doctest::Approx(0.0f));
    for (int i = 0; i < 10; ++i, t += 0.1f) {
        CHECK(step(*l, t) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(0.0f));
        CHECK_FALSE(moving(d, "Child"));
    }
}

TEST_CASE("T-717: leaving the hold (seek, replay, reverse, release) evaluates as before") {
    auto l = load(doc(kSlideStyle));
    Document& d = *l->doc;
    const float t = playToHeldEnd(*l);
    CHECK(step(*l, t) == 0);  // The Document's clock is now t.

    SUBCASE("seek back to the middle") {
        REQUIRE(d.seekTimeline("", "slide", 0.25f) == Status::kOk);
        CHECK(step(*l, t + 0.05f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.5f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(10.0f));
        // Seeking back to the end: written once more, then held again.
        REQUIRE(d.seekTimeline("", "slide", 0.5f) == Status::kOk);
        CHECK(step(*l, t + 0.1f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(step(*l, t + 0.2f) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
    }
    SUBCASE("play again from the start") {
        REQUIRE(d.playTimeline("", "slide", holdOptions()) == Status::kOk);
        CHECK(step(*l, t + 0.25f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.5f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(10.0f));
        CHECK(moving(d, "Child"));
    }
    SUBCASE("play reversed") {
        REQUIRE(d.playTimeline("", "slide", holdOptions(/*reverse=*/true)) == Status::kOk);
        CHECK(step(*l, t + 0.25f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.5f));
        CHECK(step(*l, t + 0.6f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.0f));
        CHECK(step(*l, t + 0.7f) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(0.0f));
    }
    SUBCASE("release") {
        REQUIRE(d.stopTimeline("", "slide") == Status::kOk);
        CHECK(step(*l, t + 0.1f) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));  // The styled value.
        CHECK(translateXOf(d, "Box") == doctest::Approx(0.0f));
        CHECK(l->perf.current().activeAnimations == 0);
    }
}

TEST_CASE("T-717: a restyle of a held target keeps the held values") {
    auto l = load(doc(kSlideStyle));
    Document& d = *l->doc;
    float t = playToHeldEnd(*l);
    CHECK(step(*l, t) == 0);

    SUBCASE("class change before the frame") {
        REQUIRE(d.byId("Box").addClass("faded"));
        CHECK(step(*l, t + 0.1f) == 1);  // Re-applied in the restyle's frame.
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(20.0f));
        CHECK_FALSE(moving(d, "Child"));
        CHECK(step(*l, t + 0.2f) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(20.0f));
        REQUIRE(d.byId("Box").removeClass("faded"));
        step(*l, t + 0.3f);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(20.0f));
    }
    SUBCASE("class change after the Animator ran (the frame's late style pass)") {
        // Another group's End listener restyles the Box after evaluate() (the frame's extra style
        // pass): its cascade overwrites the held opacity in that frame, exactly as before T-717,
        // and the next frame puts the held values back.
        d.setTimelineListener([&](const TimelineEvent& e) {
            if (e.kind == TimelineEventKind::End) {
                d.byId("Box").addClass("faded");
            }
        });
        REQUIRE(d.playTimeline("", "blink") == Status::kOk);
        CHECK(step(*l, t + 0.1f) == 1);  // blink only.
        step(*l, t + 0.4f);              // blink ends; the listener adds the class.
        REQUIRE(d.byId("Box").hasClass("faded"));
        CHECK(step(*l, t + 0.5f) == 1);  // The held member re-applies its values.
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
        CHECK(translateXOf(d, "Box") == doctest::Approx(20.0f));
        CHECK(step(*l, t + 0.6f) == 0);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
    }
    SUBCASE("a direct write to the property is overwritten again, as before") {
        d.byId("Box").setProperty(prop::Opacity, Value::ofFloat(0.2f));
        CHECK(step(*l, t + 0.1f) == 1);
        CHECK(opacityOf(d, "Box") == doctest::Approx(1.0f));
    }
}

TEST_CASE("T-717: a held member still wins over the element's own CSS animation") {
    auto l = load(doc(R"(
      @keyframes dim { from { opacity: 0.5 } to { opacity: 0.5 } }
      #A { animation: dim 10s linear; }
      @timeline show { #A { 0s { opacity: 1 } 1s { opacity: 0 } } }
    )",
                      "<div id=\"A\"></div>"));
    Document& d = *l->doc;
    step(*l, 0.1f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    REQUIRE(d.playTimeline("", "show", holdOptions()) == Status::kOk);
    step(*l, 0.35f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.75f));
    step(*l, 1.5f);
    for (float t = 1.6f; t < 2.5f; t += 0.1f) {
        CHECK(step(*l, t) == 2);  // The CSS animation's write would replace the held one.
        CHECK(opacityOf(d, "A") == doctest::Approx(0.0f));
    }
}

TEST_CASE("T-717: a CSS animation of another property on a held target does not wake the member") {
    auto l = load(doc(R"(
      @keyframes spin { from { transform: rotate(0deg) } to { transform: rotate(360deg) } }
      @keyframes slide { from { transform: translateX(0px) } to { transform: translateX(10px) } }
      #A { animation: spin 10s linear infinite; }
      #B { animation: slide 10s linear infinite; }
      @timeline show {
        #A { 0s { opacity: 1 } 1s { opacity: 0 } }
        #B { 0s { transform: translate(0px, 0px) } 1s { transform: translate(4px, 8px) } }
      }
    )",
                      "<div id=\"A\"></div><div id=\"B\"></div>"));
    Document& d = *l->doc;
    REQUIRE(d.playTimeline("", "show", holdOptions()) == Status::kOk);
    step(*l, 0.5f);
    step(*l, 1.5f);
    // #A: rotate vs opacity -- held. #B: translateX overlaps the member's translate -- not held, and
    // the member's translate still wins, as before.
    for (float t = 1.6f; t < 2.5f; t += 0.1f) {
        CHECK(step(*l, t) == 3);  // spin, slide and the #B member.
        CHECK(opacityOf(d, "A") == doctest::Approx(0.0f));
        CHECK(translateXOf(d, "B") == doctest::Approx(4.0f));
        CHECK(d.byId("B").property(prop::TranslateY).f == doctest::Approx(8.0f));
    }
}

TEST_CASE("T-717: a held 3D track keeps its element a 3D layer across restyles") {
    auto l = load(doc(R"(
      #Box { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }
      #Child { position: absolute; left: 1px; top: 1px; width: 10px; height: 10px; }
      .faded { opacity: 0.3; }
      @timeline tilt { #Box { 0s { transform: rotateY(0deg) } .5s { transform: rotateY(30deg) } } }
      @timeline blink { #Child { 0s { opacity: 0 } .2s { opacity: 1 } } }
    )"));
    Document& d = *l->doc;
    const NodeId box = d.byId("Box").node;
    REQUIRE(d.playTimeline("", "tilt", holdOptions()) == Status::kOk);
    step(*l, 0.25f);
    step(*l, 0.6f);
    CHECK(d.arena().common(box).is3D == 1);
    CHECK(step(*l, 0.7f) == 0);
    CHECK(d.arena().common(box).is3D == 1);
    SUBCASE("restyle before the frame") {
        REQUIRE(d.byId("Box").addClass("faded"));
        CHECK(step(*l, 0.8f) == 1);
        CHECK(d.arena().common(box).is3D == 1);
        CHECK(d.byId("Box").property(prop::RotationY).f == doctest::Approx(30.0f));
    }
    SUBCASE("restyle in the frame's late style pass") {
        d.setTimelineListener([&](const TimelineEvent& e) {
            if (e.kind == TimelineEventKind::End) {
                d.byId("Box").addClass("faded");
            }
        });
        REQUIRE(d.playTimeline("", "blink") == Status::kOk);
        step(*l, 0.8f);
        step(*l, 1.1f);  // blink ends; the Box is restyled after the Animator ran.
        REQUIRE(d.byId("Box").hasClass("faded"));
        CHECK(d.arena().common(box).is3D == 1);
        step(*l, 1.2f);
        CHECK(d.arena().common(box).is3D == 1);
        CHECK(d.byId("Box").property(prop::RotationY).f == doctest::Approx(30.0f));
    }
}

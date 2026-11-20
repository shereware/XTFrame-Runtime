#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "mini_css.h"  // Code/Runtime/core/src (internal; a test-only include dir).
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-756 (Docs/design/05-style-and-css.md §5.6, CSS Animations 1): the eight `animation-*`
/// longhands. Each one cascades on its own (the shorthand expands into all eight), lists combine by
/// `animation-name`'s length, and the result is the existing `animation` list: a document that
/// says the same thing with the shorthand plays the same list (T-778: combined at run time).

using namespace xt;
using namespace xt::compile;

namespace {

constexpr const char* kKeyframes =
    "@keyframes fade { from { opacity: 0.2 } to { opacity: 0.6 } }"
    "@keyframes a { from { opacity: 1 } to { opacity: 0 } }"
    "@keyframes b { from { width: 0px } to { width: 100px } }"
    "@keyframes c { from { height: 0px } to { height: 100px } }";

std::string page(std::string_view style, std::string_view body) {
    // The body on a line of its own, so the source map's element columns do not depend on `style`.
    return "<xt><head><style>" + std::string(kKeyframes) + std::string(style) +
           "</style></head>\n<body id=\"Root\">" + std::string(body) + "</body></xt>";
}

CompileResult compileText(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    return result;
}

bool hasWarning(const CompileResult& result, std::string_view needle) {
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// The IR of `#x { <decls> }` on one element.
std::vector<uint8_t> irOf(std::string_view decls) {
    const CompileResult result =
        compileText(page("#x { " + std::string(decls) + " }", "<div id=\"x\">x</div>"));
    REQUIRE(result.ok());
    return result.ir;
}

/// The animation list `#x` plays for `#x { <decls> }` (T-778: combined at run time).
std::vector<IrAnimationSpec> playedOf(std::string_view decls) {
    const std::vector<uint8_t> ir = irOf(decls);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 200});
    doc->update(0.0f);
    const ComputedStyle& style = doc->arena().element(doc->byId("x").node).style;
    uint32_t offset = style.animationOffset;
    std::span<const IrAnimationSpec> pool = doc->ir().style().animations();
    if ((offset & kRuntimeEffectOffsetBit) != 0) {
        offset &= ~kRuntimeEffectOffsetBit;
        pool = doc->runtimeAnimations();
    }
    REQUIRE(uint64_t{offset} + style.animationCount <= pool.size());
    return std::vector<IrAnimationSpec>(pool.begin() + offset, pool.begin() + offset + style.animationCount);
}

/// `longhands` and `shorthand` play the same list (T-756 compared the bytes; since T-778 a block
/// without the shorthand compiles to its own longhands, combined at run time).
void sameIr(std::string_view longhands, std::string_view shorthand) {
    CAPTURE(longhands);
    CAPTURE(shorthand);
    const std::vector<IrAnimationSpec> a = playedOf(longhands);
    const std::vector<IrAnimationSpec> b = playedOf(shorthand);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CAPTURE(i);
        CHECK(a[i].timeline == b[i].timeline);
        CHECK(a[i].duration == b[i].duration);
        CHECK(a[i].delay == b[i].delay);
        CHECK(std::memcmp(&a[i].easing, &b[i].easing, sizeof(IrEasing)) == 0);
        CHECK(a[i].iterations == b[i].iterations);
        CHECK(a[i].direction == b[i].direction);
        CHECK(a[i].fill == b[i].fill);
        CHECK(a[i].playState == b[i].playState);
    }
}

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    void stepTo(float t) { doc->update(t); }
    float opacity(std::string_view id) const { return doc->byId(id).property(prop::Opacity).f; }
};

Ui open(const std::string& xt) {
    Ui out;
    out.result = compileText(xt);
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    out.doc->update(0.0f);
    return out;
}

/// `#x`'s opacity at `t` seconds for `style` (`fade` runs 0.2 -> 0.6, linearly).
float opacityAt(std::string_view style, std::string_view body, float t) {
    Ui ui = open(page(style, body));
    ui.stepTo(t);
    return ui.opacity("x");
}

constexpr const char* kCard = R"X(<div id="x" class="card late" style="width: 10px; height: 10px">x</div>)X";

}  // namespace

TEST_CASE("T-756: each longhand with animation-name compiles to the shorthand's bytes") {
    // Unset longhands take the shorthand's defaults: 0s, linear, 0s, 1, normal, none, running.
    sameIr("animation-name: fade; animation-duration: 2s", "animation: fade 2s");
    sameIr("animation-name: fade; animation-duration: 2s; animation-timing-function: ease-in",
           "animation: fade 2s ease-in");
    sameIr("animation-name: fade; animation-timing-function: steps(4, start)",
           "animation: fade 0s steps(4, start)");
    sameIr("animation-name: fade; animation-delay: .3s", "animation: fade 0s .3s");
    sameIr("animation-name: fade; animation-iteration-count: infinite", "animation: fade 0s infinite");
    sameIr("animation-name: fade; animation-iteration-count: 3", "animation: fade 0s 3");
    sameIr("animation-name: fade; animation-direction: alternate", "animation: fade 0s alternate");
    sameIr("animation-name: fade; animation-fill-mode: both", "animation: fade 0s both");
    sameIr("animation-name: fade; animation-play-state: paused", "animation: fade 0s paused");
    // All eight, in another order.
    sameIr(
        "animation-play-state: paused; animation-fill-mode: forwards; animation-direction: reverse;"
        "animation-iteration-count: 2; animation-delay: 1s; animation-timing-function: ease-out;"
        "animation-duration: 3s; animation-name: fade",
        "animation: fade 3s ease-out 1s 2 reverse forwards paused");
    // `animation-name: none` alone: no animation, like `animation: none`.
    sameIr("animation-name: none", "animation: none");
}

TEST_CASE("T-756: lists combine by animation-name's length") {
    // A shorter list repeats: durations 1s, 2s, 1s.
    sameIr("animation-name: a, b, c; animation-duration: 1s, 2s", "animation: a 1s, b 2s, c 1s");
    // A longer list is cut: one name, the first duration.
    sameIr("animation-name: a; animation-duration: 1s, 2s, 3s", "animation: a 1s");
    // `none` items do not play but keep their place in the other lists.
    sameIr("animation-name: a, none, b; animation-duration: 1s, 2s, 3s", "animation: a 1s, b 3s");
    // An iteration count of 0 does not play either (T-272).
    sameIr("animation-name: a, b; animation-duration: 1s; animation-iteration-count: 0, 2",
           "animation: b 1s 2");
}

TEST_CASE("T-756: shorthand and longhands in one rule, in either order") {
    // The shorthand sets all eight, the later delay changes one: fade 1s .3s.
    sameIr("animation: fade 1s; animation-delay: .3s", "animation: fade 1s .3s");
    // The later shorthand resets the delay: fade 1s.
    sameIr("animation-delay: .3s; animation: fade 1s", "animation: fade 1s");
    // A later name list keeps the shorthand's other fields, repeated: a 1s .5s, b 1s .5s.
    sameIr("animation: fade 1s .5s; animation-name: a, b", "animation: a 1s .5s, b 1s .5s");
    // A shorthand-only document is untouched.
    sameIr("animation: fade 1s, a 2s", "animation: fade 1s, a 2s");
}

TEST_CASE("T-756: an invalid longhand is ignored with a warning, as CSS ignores the declaration") {
    const CompileResult result =
        compileText(page("#x { animation: fade 1s; animation-delay: infinite }", "<div id=\"x\">x</div>"));
    REQUIRE(result.ok());
    CHECK(hasWarning(result, "invalid value for \"animation-delay\""));
    CHECK(result.ir == irOf("animation: fade 1s"));
}

TEST_CASE("T-756: longhands in another rule cascade with the base rule's shorthand") {
    // Expected opacities: fade 1s linear, 0.2 -> 0.6, no fill. At 0.75 s without a delay: 0.5; with
    // a 0.5 s delay: 0.3.
    // `.card.late` implies `.card`: fade 1s, delay .5s.
    CHECK(opacityAt(".card { animation: fade 1s } .card.late { animation-delay: .5s }", kCard, 0.75f) ==
          doctest::Approx(0.3f));
    // `#x` matches only an element that also has `.card`: fade 1s, delay .5s.
    CHECK(opacityAt(".card { animation: fade 1s } #x { animation-delay: .5s }", kCard, 0.75f) ==
          doctest::Approx(0.3f));
    // Specificity, not order: `#x`'s delay outranks `.card`'s shorthand written after it.
    CHECK(opacityAt("#x { animation-delay: .5s } .card { animation: fade 1s }", kCard, 0.75f) ==
          doctest::Approx(0.3f));
    // The other way round: `#x`'s shorthand resets the delay and outranks `.card`'s longhand, which
    // has no name of its own to apply to (warned, ignored): fade 1s, no delay.
    CHECK(opacityAt(".card { animation-delay: .5s } #x { animation: fade 1s }", kCard, 0.75f) ==
          doctest::Approx(0.5f));
    // `.late` does not imply `.card` by its text, but every element with `.late` here also has
    // `.card`: fade 1s, delay .5s.
    CHECK(opacityAt(".card { animation: fade 1s } .late { animation-delay: .5s }", kCard, 0.75f) ==
          doctest::Approx(0.3f));
    // Inline style: fade 1s from `.card`, delay .5s from the attribute.
    CHECK(opacityAt(
              ".card { animation: fade 1s }",
              R"X(<div id="x" class="card" style="width: 10px; height: 10px; animation-delay: .5s">x</div>)X",
              0.75f) == doctest::Approx(0.3f));
}

TEST_CASE("T-756: longhands inside @media override the base rule only while the query holds") {
    // Viewport 400 px wide.
    CHECK(
        opacityAt(".card { animation: fade 1s } @media (min-width: 100px) { .card { animation-delay: .5s } }",
                  kCard, 0.75f) == doctest::Approx(0.3f));
    CHECK(opacityAt(
              ".card { animation: fade 1s } @media (min-width: 1000px) { .card { animation-delay: .5s } }",
              kCard, 0.75f) == doctest::Approx(0.5f));
}

TEST_CASE("T-756: a longhand with no animation-name anywhere plays nothing") {
    // T-778: a style property like any other -- no compile-time warning any more.
    const CompileResult result =
        compileText(page(".z { animation-delay: .5s }", "<div id=\"x\" class=\"z\">x</div>"));
    REQUIRE(result.ok());
    CHECK_FALSE(hasWarning(result, "animation-name"));
    CHECK(playedOf("animation-delay: .5s").empty());
}

TEST_CASE("T-756: animation-delay starts the animation after the delay (runtime)") {
    // fade 1s linear, delay .5s: at 0.25 s nothing plays (opacity stays 1, fill none); at 1.0 s it is
    // halfway (0.4).
    Ui ui = open(page("#x { animation-name: fade; animation-duration: 1s; animation-delay: .5s }",
                      R"X(<div id="x" style="width: 10px; height: 10px">x</div>)X"));
    ui.stepTo(0.25f);
    CHECK(ui.opacity("x") == doctest::Approx(1.0f));
    ui.stepTo(1.0f);
    CHECK(ui.opacity("x") == doctest::Approx(0.4f));
}

TEST_CASE("T-756: a state rule that only sets animation-play-state pauses the animation") {
    // `.card:hover` implies `.card`: fade 2s infinite, paused while hovered.
    Ui ui = open(page(".card { animation: fade 2s infinite } .card:hover { animation-play-state: paused }",
                      R"X(<div id="x" class="card" style="width: 10px; height: 10px">x</div>)X"));
    ui.stepTo(0.5f);
    const float before = ui.opacity("x");
    CHECK(before == doctest::Approx(0.3f));
    REQUIRE(ui.doc->setCustomState(ui.doc->byId("x"), "Hover", true) == Status::kOk);
    ui.stepTo(0.5f);
    ui.stepTo(1.5f);
    CHECK(ui.opacity("x") == doctest::Approx(before));
    REQUIRE(ui.doc->setCustomState(ui.doc->byId("x"), "Hover", false) == Status::kOk);
    ui.stepTo(1.5f);
    ui.stepTo(2.0f);  // Resumed, not restarted: 0.5 s of play after 0.5 s.
    CHECK(ui.opacity("x") == doctest::Approx(0.4f));
}

TEST_CASE("T-756: mini_css combines the longhands of one setStyle text") {
    std::vector<mini_css::Decl> decls;
    CHECK(mini_css::parseDeclarations("animation: fade 1s; animation-delay: 2s", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].animationMask == 0);
    REQUIRE(decls[0].animations.size() == 1);
    CHECK(decls[0].animations[0].name == "fade");
    CHECK(decls[0].animations[0].spec.duration == doctest::Approx(1.0f));
    CHECK(decls[0].animations[0].spec.delay == doctest::Approx(2.0f));

    decls.clear();
    CHECK(mini_css::parseDeclarations("animation-play-state: paused; animation-duration: 1s, 2s", decls) ==
          Status::kOk);
    REQUIRE(decls.size() == 1);
    CHECK(decls[0].animationMask == ((1u << 7) | (1u << 1)));
    CHECK(decls[0].animations.empty());

    decls.clear();
    CHECK(mini_css::parseDeclarations("animation-delay: infinite", decls) == Status::kInvalidArgument);
    CHECK(decls.empty());
}

TEST_CASE("T-756: setStyle with longhands only changes the element's current animation") {
    Ui ui = open(page("#x { animation: fade 2s infinite }",
                      R"X(<div id="x" style="width: 10px; height: 10px">x</div>)X"));
    ui.stepTo(0.5f);
    CHECK(ui.opacity("x") == doctest::Approx(0.3f));
    const std::vector<uint32_t> before = ui.doc->animationsOf(ui.doc->byId("x"));
    REQUIRE(ui.doc->byId("x").setStyle("animation-play-state: paused") == Status::kOk);
    ui.stepTo(0.5f);
    ui.stepTo(1.5f);
    CHECK(ui.opacity("x") == doctest::Approx(0.3f));
    CHECK(ui.doc->animationsOf(ui.doc->byId("x")) == before);  // The same instance, paused.
    REQUIRE(ui.doc->byId("x").setStyle("animation-play-state: running") == Status::kOk);
    ui.stepTo(1.5f);
    ui.stepTo(2.0f);
    CHECK(ui.opacity("x") == doctest::Approx(0.4f));
    // The same text again changes nothing (no restart).
    REQUIRE(ui.doc->byId("x").setStyle("animation-play-state: running") == Status::kOk);
    ui.stepTo(2.0f);
    CHECK(ui.opacity("x") == doctest::Approx(0.4f));
}

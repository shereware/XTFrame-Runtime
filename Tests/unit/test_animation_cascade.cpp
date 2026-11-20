#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "style_decode.h"  // Code/Runtime/core/src (internal; a test-only include dir).
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-778 (Docs/design/05-style-and-css.md §5.6, the 2026-11-18 revision): the `animation-*` and
/// `transition-*` longhands are style properties of their own (StyleProp 282-293). Each cascades
/// like any other property -- specificity, source order, `!important`, states, `@media` -- and
/// the element's animation / transition list is combined from the eight / four results, after the
/// cascade, by `animation-name` / `transition-property`'s length. Rules that do not imply each
/// other now merge, as in CSS. The replay rules are unchanged (test_animation_identity.cpp).

using namespace xt;
using namespace xt::compile;
namespace sp = style_decode::styleprop;

namespace {

constexpr const char* kKeyframes =
    "@keyframes fade { from { opacity: 0.2 } to { opacity: 0.6 } }"
    "@keyframes a { from { opacity: 1 } to { opacity: 0 } }"
    "@keyframes b { from { width: 0px } to { width: 100px } }"
    "@keyframes c { from { height: 0px } to { height: 100px } }";

/// Every `div` a 10 px box at the origin (`#other` elsewhere), from a tag rule any test rule outranks.
constexpr const char* kBase =
    "div { position: absolute; left: 0px; top: 0px; width: 10px; height: 10px } #other { left: 200px; top: "
    "200px }";

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(kKeyframes) + kBase + std::string(style) +
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

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    void stepTo(float t) { doc->update(t); }
    float prop(std::string_view id, PropId p) const {
        const Value v = doc->byId(id).property(p);
        return v.type == ValueType::Length ? v.len.value : v.f;
    }
    float opacity(std::string_view id) const { return prop(id, prop::Opacity); }
    const ComputedStyle& style(std::string_view id) const {
        return doc->arena().element(doc->byId(id).node).style;
    }
    /// The element's combined `animation` list.
    std::vector<IrAnimationSpec> played(std::string_view id) const {
        const ComputedStyle& s = style(id);
        uint32_t offset = s.animationOffset;
        std::span<const IrAnimationSpec> pool = doc->ir().style().animations();
        if ((offset & kRuntimeEffectOffsetBit) != 0) {
            offset &= ~kRuntimeEffectOffsetBit;
            pool = doc->runtimeAnimations();
        }
        REQUIRE(uint64_t{offset} + s.animationCount <= pool.size());
        return std::vector<IrAnimationSpec>(pool.begin() + offset, pool.begin() + offset + s.animationCount);
    }
    TimelineId timeline(std::string_view name) const {
        for (const IrKeyframesRef& ref : doc->ir().style().keyframesRefs()) {
            if (doc->string(ref.name) == name) {
                return ref.timeline;
            }
        }
        return kInvalidTimeline;
    }
    void pointer(Vec2 pos) {
        InputEvent ev;
        ev.kind = InputEvent::PointerMove;
        ev.pos = pos;
        doc->dispatchInput(ev);
    }
    bool styleDirty(std::string_view id) const {
        return (doc->arena().dirty(doc->byId(id).node) & static_cast<uint8_t>(kDirtyStyle)) != 0;
    }
};

Ui open(const std::string& xt) {
    Ui out;
    out.result = compileText(xt);
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    out.doc->update(0.0f);
    return out;
}

constexpr const char* kBoxes =
    R"X(<div id="e" class="x y chip">e</div><div id="other" class="x chip">o</div>)X";

/// `#e`'s opacity at `t` for `style` (`fade` runs 0.2 -> 0.6 linearly; opacity 1 when nothing plays).
float opacityAt(std::string_view style, float t) {
    Ui ui = open(page(style, kBoxes));
    ui.stepTo(t);
    return ui.opacity("e");
}

}  // namespace

TEST_CASE(
    "T-778: a state rule's play-state merges with an unrelated rule's animation (the T-756 "
    "counter-example)") {
    // `.chip:hover` does not imply `#e` (`#other` is a `.chip` too), so T-756 did not merge `#e`'s
    // delay into the hover rule: hovering dropped the delay. (With `animation: fade 1s` in `#e`
    // itself, as the task sheet wrote it, `#e`'s shorthand would set the play state too and outrank
    // the hover rule -- in CSS the hover does not pause that one; checked at the end.)
    Ui ui =
        open(page(".chip { animation: fade 1s } #e { animation-delay: .3s } "
                  ".chip:hover { animation-play-state: paused }",
                  kBoxes));
    ui.stepTo(0.2f);
    CHECK(ui.opacity("e") == doctest::Approx(1.0f));  // Still in the 0.3 s delay, no fill.
    ui.stepTo(0.55f);
    CHECK(ui.opacity("e") == doctest::Approx(0.3f));  // A quarter in.
    const std::vector<uint32_t> before = ui.doc->animationsOf(ui.doc->byId("e"));
    REQUIRE(before.size() == 1);
    ui.pointer(Vec2{5, 5});
    // The hover re-resolves `#e` alone (T-723's state dependency table): `.chip:hover` is a subject
    // compound, and `#other` is not hovered.
    CHECK(ui.styleDirty("e"));
    CHECK_FALSE(ui.styleDirty("other"));
    CHECK_FALSE(ui.styleDirty("Root"));
    ui.stepTo(0.55f);
    ui.stepTo(0.8f);
    CHECK(ui.opacity("e") == doctest::Approx(0.3f));  // Paused.
    CHECK(ui.doc->animationsOf(ui.doc->byId("e")) == before);
    const std::vector<IrAnimationSpec> paused = ui.played("e");
    REQUIRE(paused.size() == 1);
    CHECK(paused[0].playState == 1);
    CHECK(paused[0].delay == doctest::Approx(0.3f));
    ui.pointer(Vec2{390, 290});
    ui.stepTo(0.8f);
    ui.stepTo(0.9f);
    CHECK(ui.opacity("e") == doctest::Approx(0.34f));  // Resumed, not restarted.
    CHECK(ui.doc->animationsOf(ui.doc->byId("e")) == before);

    // The sheet's literal rules: `#e`'s shorthand outranks `.chip:hover` on the play state too.
    Ui literal = open(
        page("#e { animation: fade 1s; animation-delay: .3s } .chip:hover { animation-play-state: paused }",
             kBoxes));
    literal.stepTo(0.55f);
    literal.pointer(Vec2{5, 5});
    literal.stepTo(0.55f);
    literal.stepTo(0.8f);
    CHECK(literal.opacity("e") == doctest::Approx(0.4f));  // Still playing.
}

TEST_CASE("T-778: longhands of rules that do not imply each other cascade one by one") {
    // `fade 1s` from `div`; at 0.75 s: no delay 0.5, delay .25 -> 0.4, delay .5 -> 0.3.
    // Specificity: `#e`'s name with `.x`'s duration (`#other` is `.x` without being `#e`).
    CHECK(opacityAt("#e { animation-name: fade } .x { animation-duration: 2s }", 1.0f) ==
          doctest::Approx(0.4f));
    // Source order: `.y` after `.x`.
    CHECK(opacityAt("div { animation: fade 1s } .x { animation-delay: .5s } .y { animation-delay: .25s }",
                    0.75f) == doctest::Approx(0.4f));
    CHECK(opacityAt("div { animation: fade 1s } .y { animation-delay: .25s } .x { animation-delay: .5s }",
                    0.75f) == doctest::Approx(0.3f));
    // Specificity over order: `#e`'s delay outranks the later `.x`.
    CHECK(opacityAt("div { animation: fade 1s } #e { animation-delay: .25s } .x { animation-delay: .5s }",
                    0.75f) == doctest::Approx(0.4f));
    // `!important` over specificity.
    CHECK(opacityAt(
              "div { animation: fade 1s } .x { animation-delay: .5s !important } #e { animation-delay: 0s }",
              0.75f) == doctest::Approx(0.3f));
    // A later shorthand of higher specificity resets every longhand.
    CHECK(opacityAt(".x { animation-delay: .5s } #e { animation: fade 1s }", 0.75f) == doctest::Approx(0.5f));
    // `@media` while its query holds (400 px wide), not otherwise.
    CHECK(opacityAt("div { animation: fade 1s } @media (min-width: 100px) { .x { animation-delay: .5s } }",
                    0.75f) == doctest::Approx(0.3f));
    CHECK(opacityAt("div { animation: fade 1s } @media (min-width: 1000px) { .x { animation-delay: .5s } }",
                    0.75f) == doctest::Approx(0.5f));
    // A longhand with no `animation-name` anywhere: no animation, no warning.
    const CompileResult lone = compileText(page(".x { animation-delay: .5s }", kBoxes));
    REQUIRE(lone.ok());
    for (const Diagnostic& d : lone.diagnostics) {
        CHECK(d.message.find("animation-name") == std::string::npos);
    }
    CHECK(opacityAt(".x { animation-delay: .5s }", 0.75f) == doctest::Approx(1.0f));
}

TEST_CASE("T-778: lists of different lengths repeat and are cut across rules") {
    Ui ui =
        open(page("div { animation: a 1s, b 2s, c 3s } .x { animation-duration: 4s, 8s } "
                  ".y { animation-iteration-count: 2 }",
                  kBoxes));
    std::vector<IrAnimationSpec> list = ui.played("e");
    REQUIRE(list.size() == 3);
    CHECK(list[0].timeline == ui.timeline("a"));
    CHECK(list[0].duration == doctest::Approx(4.0f));
    CHECK(list[1].duration == doctest::Approx(8.0f));
    CHECK(list[2].duration == doctest::Approx(4.0f));  // The shorter list repeats.
    for (const IrAnimationSpec& item : list) {
        CHECK(item.iterations == 2);
    }
    // A shorter name list cuts the others; a `none` name keeps its place and plays nothing.
    Ui cut = open(page("div { animation: a 1s, b 2s, c 3s } #e { animation-name: none, c }", kBoxes));
    list = cut.played("e");
    REQUIRE(list.size() == 1);
    CHECK(list[0].timeline == cut.timeline("c"));
    CHECK(list[0].duration == doctest::Approx(2.0f));
    // An iteration count of 0 plays nothing but keeps the others aligned.
    Ui zero = open(page("div { animation: a 1s, b 2s } #e { animation-iteration-count: 0, 3 }", kBoxes));
    list = zero.played("e");
    REQUIRE(list.size() == 1);
    CHECK(list[0].timeline == zero.timeline("b"));
    CHECK(list[0].iterations == 3);
}

TEST_CASE(
    "T-778: the shorthand compiles to the eight longhands naming one list; the old declarations are gone") {
    const CompileResult result =
        compileText(page("#e { animation: fade 1s; transition: opacity .5s }", kBoxes));
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    CHECK(view.value().formatMinor() == kIrFormatMinor);
    uint32_t animation = 0;
    uint32_t transition = 0;
    for (const IrDecl& d : view.value().style().decls()) {
        CHECK(d.prop != sp::Animation);
        CHECK(d.prop != sp::Transition);
        if (sp::isAnimationLonghand(d.prop)) {
            ++animation;
            CHECK(irValueU32(d.value, 1) == 1);
        }
        if (sp::isTransitionLonghand(d.prop)) {
            ++transition;
            CHECK(irValueU32(d.value, 1) == 1);
        }
    }
    CHECK(animation == 8);
    CHECK(transition == 4);
    // A shorthand-only element plays its compiled list as it is (no combined copy).
    Ui ui = open(page("#e { animation: fade 1s }", kBoxes));
    CHECK((ui.style("e").animationOffset & kRuntimeEffectOffsetBit) == 0);
    CHECK(ui.style("e").animationCount == 1);
}

TEST_CASE("T-778: var() as a longhand's whole value") {
    CHECK(opacityAt(":root { --d: .5s; } div { animation: fade 1s } .x { animation-delay: var(--d) }",
                    0.75f) == doctest::Approx(0.3f));
    CHECK(opacityAt(":root { --t: 2s; } #e { animation-name: fade } .x { animation-duration: var(--t) }",
                    1.0f) == doctest::Approx(0.4f));
    // Undefined, no fallback: the longhand's initial value (0 s), with a warning.
    CHECK(opacityAt("div { animation: fade 1s .5s } .x { animation-delay: var(--nope) }", 0.75f) ==
          doctest::Approx(0.5f));
}

TEST_CASE("T-778: transition longhands cascade one by one") {
    // `#e`'s property list with `.x`'s duration; the hover sets the opacity to 0.
    Ui ui =
        open(page("#e { transition-property: opacity } .x { transition-duration: 1s } "
                  ".chip:hover { opacity: 0 }",
                  kBoxes));
    ui.pointer(Vec2{5, 5});
    ui.stepTo(0.0f);
    ui.stepTo(0.5f);
    CHECK(ui.opacity("e") == doctest::Approx(0.5f));
    ui.stepTo(1.0f);
    CHECK(ui.opacity("e") == doctest::Approx(0.0f));
    // Without the property list (initial `all`), `.x`'s duration alone transitions everything.
    Ui all = open(page(".x { transition-duration: 1s } .chip:hover { opacity: 0 }", kBoxes));
    all.pointer(Vec2{5, 5});
    all.stepTo(0.0f);
    all.stepTo(0.25f);
    CHECK(all.opacity("e") == doctest::Approx(0.75f));
    // `transition-property: none` of a more specific rule wins: no transition.
    Ui none = open(page(
        ".x { transition: opacity 1s } #e { transition-property: none } .chip:hover { opacity: 0 }", kBoxes));
    none.pointer(Vec2{5, 5});
    none.stepTo(0.0f);
    none.stepTo(0.25f);
    CHECK(none.opacity("e") == doctest::Approx(0.0f));
    // Lists of different lengths: `transform` is one item (several PropIds); the durations repeat.
    Ui lists =
        open(page("div { transition-property: opacity, transform, width } .x { transition-duration: 1s, 2s } "
                  ".chip:hover { opacity: 0; width: 20px }",
                  kBoxes));
    lists.pointer(Vec2{5, 5});
    lists.stepTo(0.0f);
    lists.stepTo(0.5f);
    CHECK(lists.opacity("e") == doctest::Approx(0.5f));             // 1 s.
    CHECK(lists.prop("e", prop::Width) == doctest::Approx(15.0f));  // 1 s again (the third item).
}

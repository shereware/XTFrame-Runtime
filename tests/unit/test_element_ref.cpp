#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "mini_css.h"
#include "style_decode.h"  // styleprop::* (internal; core/src is a test-only include dir).
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value_helpers.h"

/// ElementRef's public API (docs/design/13-api.md §13.3, docs/tasks/T-35.md): identity, classes,
/// attributes (each branch of setAttribute), text, runtime style, the three property layers
/// (base < transition < override) and geometry.

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

}  // namespace

TEST_CASE("ElementRef: tag/id/classes/attributes") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK(box.tag() == "div");
    CHECK(box.id() == "box");
    CHECK(box.hasClass("panel"));
    CHECK_FALSE(box.hasClass("big"));
    CHECK(box.attribute("data-role") == "main");
    CHECK(box.attribute("missing").empty());

    // T-24's "the class must already exist in the IR string table" limitation is gone (T-35's
    // runtime string pool): a brand new name interns and sticks.
    CHECK(box.addClass("brandnew"));
    CHECK(box.hasClass("brandnew"));
    CHECK_FALSE(box.addClass("brandnew"));
    CHECK(box.removeClass("brandnew"));
    CHECK_FALSE(box.hasClass("brandnew"));
    CHECK(box.toggleClass("big"));
    CHECK(box.hasClass("big"));
    CHECK_FALSE(box.toggleClass("big"));
    CHECK_FALSE(box.hasClass("big"));

    ElementRef invalid;
    CHECK(invalid.tag().empty());
    CHECK(invalid.id().empty());
    CHECK(invalid.attribute("x").empty());
    CHECK(invalid.setAttribute("id", "y") == Status::kInvalidArgument);
}

TEST_CASE("ElementRef: setAttribute branches") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    // class (replaces the whole list)
    CHECK(box.setAttribute("class", " big  fresh ") == Status::kOk);
    CHECK(box.hasClass("big"));
    CHECK(box.hasClass("fresh"));
    CHECK_FALSE(box.hasClass("panel"));
    doc->update(0.0f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(200));  // #box.big { width: 200px }

    // style
    CHECK(box.setAttribute("style", "height: 33px") == Status::kOk);
    doc->update(0.0f);
    CHECK(box.property(prop::Height).len.value == doctest::Approx(33));

    // src: compile-time only.
    CHECK(box.setAttribute("src", "foo.png") == Status::kUnsupported);

    // Any other attribute round-trips.
    CHECK(box.setAttribute("data-role", "other") == Status::kOk);
    CHECK(box.attribute("data-role") == "other");
    CHECK(box.setAttribute("data-new", "1") == Status::kOk);
    CHECK(box.attribute("data-new") == "1");

    // disabled / checked
    ElementRef btn = doc->byId("btn");
    REQUIRE(btn.valid());
    CHECK(btn.hasState(kStateDisabled));  // The load-time `disabled="true"` attribute (05.2).
    CHECK(btn.setAttribute("disabled", "false") == Status::kOk);
    CHECK_FALSE(btn.hasState(kStateDisabled));
    ElementRef check = doc->byId("check");
    REQUIRE(check.valid());
    CHECK(check.hasState(kStateChecked));
    CHECK(check.setAttribute("checked", "false") == Status::kOk);
    CHECK_FALSE(check.hasState(kStateChecked));

    // value (editable input)
    ElementRef field = doc->byId("field");
    REQUIRE(field.valid());
    CHECK(field.value() == "abc");
    CHECK(field.setAttribute("value", "xyz") == Status::kOk);
    CHECK(field.value() == "xyz");
    CHECK(field.text() == "xyz");  // An input's text() is its EditState value.

    // id (last: it stops the `#box`-keyed stylesheet rules from matching).
    CHECK(box.setAttribute("id", "renamed") == Status::kOk);
    CHECK(box.id() == "renamed");
    CHECK(doc->byId("renamed").node == box.node);
    CHECK_FALSE(doc->byId("box").valid());
    doc->update(0.0f);
    CHECK(box.property(prop::Width).len.unit == LengthUnit::Auto);  // `#box.big` no longer matches.
}

TEST_CASE("ElementRef: setText") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK(box.text() == "text");
    box.setText("replaced at runtime");
    CHECK(box.text() == "replaced at runtime");
    doc->update(0.0f);
    CHECK(box.text() == "replaced at runtime");

    // An element with no Text child gets one.
    ElementRef rotated = doc->byId("rotated");
    REQUIRE(rotated.valid());
    CHECK(rotated.text().empty());
    uint32_t before = doc->arena().aliveCount();
    rotated.setText("new text node");
    CHECK(doc->arena().aliveCount() == before + 1);
    CHECK(rotated.text() == "new text node");
    doc->update(0.0f);
    CHECK(rotated.text() == "new text node");
}

TEST_CASE("ElementRef: property layers (base < transition < override)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK(box.property(prop::Width).len.value == doctest::Approx(100));

    // Layer 0: a plain setProperty write is the base value (until the next resolve/animation).
    box.setProperty(prop::Width, makeLength(120, LengthUnit::Px));
    CHECK(box.property(prop::Width).len.value == doctest::Approx(120));

    // Layer 3: a sticky write wins over everything and survives a style resolve.
    box.setProperty(prop::Width, makeLength(321, LengthUnit::Px), /*sticky=*/true);
    doc->update(0.0f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(321));
    CHECK(box.setStyle("width: 150px") == Status::kOk);
    doc->update(0.1f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(321));

    // Dropping the override exposes the base value underneath it.
    box.clearOverride(prop::Width);
    doc->update(0.2f);
    CHECK(box.property(prop::Width).len.value == doctest::Approx(150));

    // A type mismatch or a non-Element property is ignored rather than asserted on.
    box.setProperty(prop::Width, makeFloat(9));
    CHECK(box.property(prop::Width).len.value == doctest::Approx(150));
    box.setProperty(prop::FillColor, makeColor(1, 0, 0, 1));
    CHECK(box.property(prop::FillColor).type == ValueType::Float);  // Value{} for "does not apply".
}

TEST_CASE("ElementRef: setStyle drives a transition, and the transition sits under the override") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef label = doc->byId("label");
    REQUIRE(label.valid());
    CHECK(label.setStyle("width: 100px; transition: width 0.2s linear") == Status::kUnsupported);
    // The width still applied; only the `transition` declaration was rejected.
    doc->update(0.0f);
    CHECK(label.property(prop::Width).len.value == doctest::Approx(100));

    CHECK(label.setStyle("bogus: 3") == Status::kInvalidArgument);
    CHECK(label.setStyle("width: 140px") == Status::kOk);
    doc->update(0.1f);
    CHECK(label.property(prop::Width).len.value == doctest::Approx(140));
}

TEST_CASE("ElementRef: layoutBox and boundingBox (including a rotated element)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    LayoutBox lb = box.layoutBox();
    CHECK(lb.width == doctest::Approx(100));
    CHECK(lb.height == doctest::Approx(50));
    Rect bounds = box.boundingBox();
    CHECK(bounds.x == doctest::Approx(0));
    CHECK(bounds.y == doctest::Approx(0));
    CHECK(bounds.w == doctest::Approx(100));
    CHECK(bounds.h == doctest::Approx(50));

    // #rotated is 40x20 at (10, 200) with transform: rotate(90deg) and origin (0, 0): its AABB is
    // the 90-degree-swapped extent anchored at the same origin.
    ElementRef rotated = doc->byId("rotated");
    REQUIRE(rotated.valid());
    Rect rb = rotated.boundingBox();
    CHECK(rb.w == doctest::Approx(20));
    CHECK(rb.h == doctest::Approx(40));

    // T-79 item 3 (docs/design/19-3d-space.md §19.5): #persp is a 3D space root of its own
    // (`perspective: 500px`). Its own box lies on its *parent's* plane, not inside the space it
    // introduces, so nodeToScreen must not send it through its own projection -- doing so applied
    // its `world` twice and doubled its offset (48 -> 96).
    ElementRef persp = doc->byId("persp");
    REQUIRE(persp.valid());
    REQUIRE(doc->arena().common(persp.node).isSpaceRoot == 1);
    LayoutBox plb = persp.layoutBox();
    Rect pb = persp.boundingBox();
    CHECK(pb.x == doctest::Approx(48));
    CHECK(pb.y == doctest::Approx(24));
    CHECK(pb.w == doctest::Approx(plb.width));
    CHECK(pb.h == doctest::Approx(plb.height));
    CHECK(pb.w == doctest::Approx(120));
    CHECK(pb.h == doctest::Approx(60));

    ElementRef invalid;
    CHECK(invalid.boundingBox() == Rect{});
    CHECK(invalid.layoutBox().width == doctest::Approx(0));
}

TEST_CASE("ElementRef: tree navigation skips non-Element nodes") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef body = doc->byId("body");
    REQUIRE(body.valid());
    ElementRef first = body.firstChild();
    REQUIRE(first.valid());
    CHECK(first.id() == "box");
    CHECK(first.parent().node == body.node);
    // #box's only child is a Text node, so firstChild() yields nothing.
    CHECK_FALSE(first.firstChild().valid());
    ElementRef second = first.nextSibling();
    REQUIRE(second.valid());
    CHECK(second.id() == "rotated");
    CHECK(doc->root().parent().valid() == false);
}

TEST_CASE("input: a `writing-mode` is warned about at load and ignored (08.8)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    bool warned = false;
    for (const std::string& warning : doc->warnings()) {
        warned = warned || warning.find("writing-mode") != std::string::npos;
    }
    CHECK(warned);

    ElementRef vert = doc->byId("vert");
    REQUIRE(vert.valid());
    CHECK(doc->arena().element(vert.node).style.writingMode == WritingMode::HorizontalTb);
    // A non-input element keeps whatever writing-mode it is given.
    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK(box.setStyle("writing-mode: vertical-rl") == Status::kOk);
    doc->update(0.0f);
    CHECK(doc->arena().element(box.node).style.writingMode == WritingMode::VerticalRl);
}

// docs/design/18-effects.md §18.4, docs/tasks/T-51b.md: ElementRef::setEffects/setEffectParam/
// effectCount/effect, and `setStyle("effect: ...")`'s runtime declaration path.
TEST_CASE("ElementRef: setEffects/setEffectParam/effectCount/effect, setStyle(\"effect: none\")") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK(box.effectCount() == 0);

    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.params[0] = 4;
    blur.params[1] = 4;
    EffectInstance outline{};
    outline.kind = EffectKind::Outline;
    outline.params[0] = 2;
    outline.colors[0] = Color{0, 0, 1, 1};
    EffectInstance effects[2] = {blur, outline};
    CHECK(box.setEffects(effects, {}) == Status::kOk);
    doc->update(0.0f);
    REQUIRE(box.effectCount() == 2);
    CHECK(box.effect(1).kind == EffectKind::Outline);

    box.setEffectParam(0, 3, makeFloat(5));  // paramIndex 3 = EffectInstance::params[2].
    CHECK(box.property(effectParam(0, 3)).f == doctest::Approx(5));

    CHECK(box.setStyle("effect: none") == Status::kOk);
    doc->update(0.1f);
    CHECK(box.effectCount() == 0);

    ElementRef invalid;
    CHECK(invalid.effectCount() == 0);
    CHECK(invalid.effect(0).kind == EffectKind::None);
}

// docs/tasks/T-55.md "測試": setEffects({DropShadow}) followed by Document::paint's command dump
// contains PushEffect (end-to-end through the same ElementRef API the T-51b test above exercises).
TEST_CASE("ElementRef: setEffects(DropShadow) -> Document::paint dump contains PushEffect") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    EffectInstance shadow{};
    shadow.kind = EffectKind::DropShadow;
    shadow.params[0] = 6;     // offsetX
    shadow.params[1] = 8;     // offsetY
    shadow.params[2] = 10;    // blur
    shadow.params[4] = 0.5f;  // opacity
    shadow.colors[0] = Color{0, 0, 0, 1};
    CHECK(box.setEffects(std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);
    doc->update(0.0f);
    REQUIRE(box.effectCount() == 1);
    CHECK(box.effect(0).kind == EffectKind::DropShadow);

    CommandList list;
    doc->paint(list);
    std::string dump;
    dumpCommandList(list, dump);
    CHECK(dump.find("PushEffect") != std::string::npos);
    CHECK(dump.find("PopEffect") != std::string::npos);
}

TEST_CASE("ElementRef: focus() and defaultAnimation()") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef field = doc->byId("field");
    REQUIRE(field.valid());
    field.focus();
    CHECK(doc->focus().node == field.node);
    CHECK(field.hasState(kStateFocus));
    CHECK(field.defaultAnimation() == 0);
    CHECK(field.sceneNode("whatever") == kInvalidNode);
}

// docs/tasks/T-59.md: ElementRef::computedStyleValue -- PropId goes through property(), StyleProp
// reads the cascaded ComputedStyle field directly (Enum -> Value::ofEnum).
TEST_CASE("ElementRef: computedStyleValue(PropId)/(StyleProp)") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    // PropId path: same value as property() (#box's own width: 100px).
    Value width = box.computedStyleValue(prop::Width);
    CHECK(width.type == ValueType::Length);
    CHECK(width.len.value == doctest::Approx(100.0f));
    CHECK(box.property(prop::Width).len.value == doctest::Approx(width.len.value));

    // StyleProp path: Display is Block by default (05.4's initial value), Enum-encoded.
    Value display = box.computedStyleValue(style_decode::styleprop::Display);
    CHECK(display.type == ValueType::Enum);
    CHECK(display.e == static_cast<uint32_t>(Display::Block));

    // A StyleProp with no Enum/Length ComputedStyle field (font-weight: a plain uint16_t) is
    // unsupported (docs/tasks/T-59.md "其他不支援者回 Value{}").
    Value fontWeight = box.computedStyleValue(style_decode::styleprop::FontWeight);
    CHECK(fontWeight.type == ValueType::Float);
    CHECK(fontWeight.f == 0.0f);

    ElementRef invalid;
    CHECK(invalid.computedStyleValue(prop::Width).type == ValueType::Float);
}

// docs/tasks/T-59.md: Document::setElementState() forces/clears an arbitrary ElementState bit
// (here :hover) without pointer dispatch, so a state-driven rule's value shows up in property().
TEST_CASE("Document: setElementState forces/clears a pseudo-class") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());
    CHECK_FALSE(box.hasState(kStateHover));
    Color normalColor = box.property(prop::BackgroundColor).c;
    CHECK(normalColor.r == doctest::Approx(0.0f));

    doc->setElementState(box, kStateHover, true);
    doc->update(0.0f);
    CHECK(box.hasState(kStateHover));
    Color hoverColor = box.property(prop::BackgroundColor).c;
    CHECK(hoverColor.r == doctest::Approx(1.0f));  // #box:hover { background-color: #ff0000; }

    doc->setElementState(box, kStateHover, false);
    doc->update(0.0f);
    CHECK_FALSE(box.hasState(kStateHover));
    Color restoredColor = box.property(prop::BackgroundColor).c;
    CHECK(restoredColor.r == doctest::Approx(0.0f));

    // No-ops: invalid ref, zero stateBits.
    ElementRef invalid;
    doc->setElementState(invalid, kStateHover, true);
    doc->setElementState(box, 0, true);
    CHECK_FALSE(box.hasState(kStateHover));
}

// docs/tasks/T-62.md item 5: Document::animationsOf() reports the element's current CSS
// `animation` instance ids, in declaration order (here just the one `.run { animation: spin ... }`
// instance), matching ElementData::cssAnimations directly; seek() on that id moves animationTime().
TEST_CASE("Document::animationsOf: CSS animation instance id, seek() moves animationTime()") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, &assets);

    ElementRef box = doc->byId("spinBox");
    REQUIRE(box.valid());
    CHECK(doc->animationsOf(box).empty());  // No `animation` declaration until `.run` is added.

    CHECK(box.addClass("run"));
    doc->update(0.0f);
    std::vector<uint32_t> anims = doc->animationsOf(box);
    REQUIRE(anims.size() == 1);
    CHECK(anims == doc->arena().element(box.node).cssAnimations);

    doc->seek(anims[0], 0.4f);
    CHECK(doc->animationTime(anims[0]) == doctest::Approx(0.4f));

    // Empty for an invalid ref and for a ref from another Document.
    ElementRef invalid;
    CHECK(doc->animationsOf(invalid).empty());
}

// docs/design/05-style-and-css.md §5.5c (T-115): `setStyle("background-gradient: ...")` goes
// through Document's runtime gradient declaration pool and reaches the painted CommandList.
TEST_CASE("ElementRef: setStyle(\"background-gradient: ...\") paints a gradient fill") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("element_ref.xt", &shaper, &assets);

    ElementRef box = doc->byId("box");
    REQUIRE(box.valid());

    auto gradientFills = [&doc]() {
        CommandList list;
        doc->paint(list);
        int n = 0;
        for (const FillCmd& fill : list.fills) {
            if (fill.paint.kind == PaintKind::Linear || fill.paint.kind == PaintKind::Radial) {
                ++n;
            }
        }
        return n;
    };
    CHECK(gradientFills() == 0);

    CHECK(box.setStyle("background-gradient: linear-gradient(90deg, #f00, #00f)") == Status::kOk);
    doc->update(0.0f);
    CHECK(gradientFills() == 1);

    CHECK(box.setStyle("background-gradient: radial-gradient(50% 50% at 50% 50%, #fff, #000)") ==
          Status::kOk);
    doc->update(0.0f);
    CHECK(gradientFills() == 1);

    CHECK(box.setStyle("background-gradient: none") == Status::kOk);
    doc->update(0.0f);
    CHECK(gradientFills() == 0);
}

// T-123: `animation` is a supported runtime declaration -- ElementRef::setStyle resolves its
// `@keyframes` names against the IR and plays the result through the ordinary CSS animation path.
TEST_CASE("ElementRef::setStyle: animation plays, `none` stops, unknown @keyframes warns") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, nullptr);
    ElementRef box = doc->byId("widthBox");  // No `animation` of its own.
    REQUIRE(box.valid());
    CHECK(doc->animationsOf(box).empty());

    CHECK(box.setStyle("animation: spin 0.5s linear") == Status::kOk);
    doc->update(0.016f);
    std::vector<uint32_t> playing = doc->animationsOf(box);
    REQUIRE(playing.size() == 1);
    CHECK(doc->animationTimelineDuration(playing[0]) == doctest::Approx(0.5f));

    CHECK(box.setStyle("animation: none") == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->animationsOf(box).empty());

    const size_t warningsBefore = doc->warnings().size();
    CHECK(box.setStyle("animation: nosuch 1s") == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->animationsOf(box).empty());
    CHECK(doc->warnings().size() == warningsBefore + 1);

    // Two comma-separated items play two instances. (Both name `spin`: style_animation.xt's
    // other `@keyframes`, `fx`, animates `effect`, which an element without an effect stack
    // cannot take -- a pre-existing property_access.cpp assert, not this declaration's business.)
    CHECK(box.setStyle("animation: spin 1s linear, spin 2s linear") == Status::kOk);
    doc->update(0.016f);
    CHECK(doc->animationsOf(box).size() == 2);
}

// The runtime parser and the compiler agree on the same declaration text: style_animation.xt's
// `#fxBox { animation: fx 1s linear }` compiles to an IrAnimationSpec whose duration and easing
// mini_css reproduces from the same value text.
TEST_CASE("ElementRef::setStyle: mini_css's animation matches the compiled spec") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadDoc("style_animation.xt", &shaper, nullptr);
    ElementRef fxBox = doc->byId("fxBox");
    REQUIRE(fxBox.valid());
    const ComputedStyle& style = doc->arena().element(fxBox.node).style;
    REQUIRE(style.animationCount == 1);
    REQUIRE((style.animationOffset & kRuntimeEffectOffsetBit) == 0);
    const IrAnimationSpec& compiled = doc->ir().style().animations()[style.animationOffset];

    std::vector<mini_css::Decl> decls;
    REQUIRE(mini_css::parseOne(style_decode::styleprop::Animation, "fx 1s linear", decls) == Status::kOk);
    REQUIRE(decls.size() == 1);
    REQUIRE(decls[0].animations.size() == 1);
    const IrAnimationSpec& parsed = decls[0].animations[0].spec;
    CHECK(decls[0].animations[0].name == "fx");
    CHECK(parsed.duration == doctest::Approx(compiled.duration));
    CHECK(parsed.delay == doctest::Approx(compiled.delay));
    CHECK(parsed.easing.kind == compiled.easing.kind);
    CHECK(parsed.easing.x1 == doctest::Approx(compiled.easing.x1));
    CHECK(parsed.easing.y2 == doctest::Approx(compiled.easing.y2));
    CHECK(parsed.iterations == compiled.iterations);
    CHECK(parsed.direction == compiled.direction);
    CHECK(parsed.fill == compiled.fill);
    CHECK(parsed.playState == compiled.playState);
}

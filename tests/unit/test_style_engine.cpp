// GCC 12 reports -Wmissing-field-initializers for designated initializers that omit trailing
// members (fixed in later GCC releases); the omitted members are value-initialized as intended.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "doctest.h"
#include "style_decode.h"  // xt::style_decode::styleprop::* (core/src, test-only include dir).
#include "xt/animator.h"
#include "xt/compile/ir_writer.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/style_engine.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// Tests for StyleEngine::resolve(): initial values, cascade (specificity/order/!important/
/// inline), inheritance, em resolution, state-driven re-resolution, transition registration and
/// CSS `animation` play/stop, non-animatable dirty bits, and dirty propagation. See
/// docs/design/05-style-and-css.md §5.4, §5.7-5.8, docs/tasks/T-24.md "test_style_engine.cpp".
///
/// The IR is built directly with IrWriter (STYL rules/decls/transitions/animations, DOCT tree):
/// exact binary control, no CSS/HTML text parsing involved (this module doesn't need xt_compile at
/// runtime, only at test-build time, for IrWriter itself).

using namespace xt;
using namespace xt::compile;
using namespace xt::style_decode;

namespace {

IrEasing linearIrEasing() { return IrEasing{0, 1, 1, 0, 0, 1, 1}; }

IrDecl declFloat(uint16_t prop, float v, bool important = false) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Float);
    d.important = important ? 1 : 0;
    irSetValueF32(d.value, 0, v);
    return d;
}

IrDecl declLength(uint16_t prop, float v, LengthUnit unit, bool important = false) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Length);
    d.important = important ? 1 : 0;
    irSetValueF32(d.value, 0, v);
    d.value[4] = static_cast<uint8_t>(unit);
    return d;
}

IrDecl declColor(uint16_t prop, Color c, bool important = false) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Color);
    d.important = important ? 1 : 0;
    irSetValueF32(d.value, 0, c.r);
    irSetValueF32(d.value, 1, c.g);
    irSetValueF32(d.value, 2, c.b);
    irSetValueF32(d.value, 3, c.a);
    return d;
}

IrDecl declEnum(uint16_t prop, uint32_t v, bool important = false) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::Enum);
    d.important = important ? 1 : 0;
    irSetValueU32(d.value, 0, v);
    return d;
}

IrDecl declListRef(uint16_t prop, uint32_t offset, uint32_t count, bool important = false) {
    IrDecl d{};
    d.prop = prop;
    d.valueType = static_cast<uint8_t>(ValueType::ListRef);
    d.important = important ? 1 : 0;
    irSetValueU32(d.value, 0, offset);
    irSetValueU32(d.value, 1, count);
    return d;
}

/// Adds a single-part (no ancestor combinators) rule matching `classId` (kInvalidString = none)
/// and/or `idVal`, optionally requiring `stateMask`, with the given decls.
void addRule(StyleBuilder& style, StringId idVal, StringId classId, uint16_t stateMask, uint32_t specificity,
             std::vector<IrDecl> decls) {
    IrSelPart part{};
    part.combinator = 0;
    part.tag = kInvalidString;
    part.id = idVal;
    part.stateMask = stateMask;
    if (classId != kInvalidString) {
        part.classCount = 1;
        part.classOffset = static_cast<uint32_t>(style.classIds.size());
        style.classIds.push_back(classId);
    }
    uint32_t selOffset = static_cast<uint32_t>(style.selectorParts.size());
    style.selectorParts.push_back(part);

    IrRule rule{};
    rule.selOffset = selOffset;
    rule.selCount = 1;
    rule.specificity = specificity;
    rule.declOffset = static_cast<uint32_t>(style.decls.size());
    for (const IrDecl& d : decls) {
        style.decls.push_back(d);
    }
    rule.declCount = static_cast<uint32_t>(style.decls.size()) - rule.declOffset;
    style.rules.push_back(rule);
}

struct DocNodeSpec {
    std::string tag = "div";
    std::string id;  // empty = none
    std::vector<std::string> classes;
    int parent = -1;  // index into the spec list; -1 = body
    std::vector<IrDecl> inlineDecls;
};

struct Built {
    std::unique_ptr<Document> doc;
    std::unordered_map<std::string, StringId> ids;
};

/// Builds `xt > body > ...` (children placed per DocNodeSpec::parent, -1 = direct child of body).
Built buildDoc(const std::vector<DocNodeSpec>& children,
               const std::function<void(IrWriter&, StyleBuilder&)>& configureStyle) {
    IrWriter w;
    DocBuilder doc;

    IrDocElement xtEl{};
    xtEl.parent = 0xFFFFFFFFu;
    xtEl.tag = w.intern("xt");
    xtEl.id = kInvalidString;
    xtEl.firstTextChild = 0xFFFFFFFFu;
    xtEl.scene = kInvalidScene;
    xtEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(xtEl);

    IrDocElement bodyEl{};
    bodyEl.parent = 0;
    bodyEl.tag = w.intern("body");
    bodyEl.id = kInvalidString;
    bodyEl.firstTextChild = 0xFFFFFFFFu;
    bodyEl.scene = kInvalidScene;
    bodyEl.cameraIndex = 0xFFFFFFFFu;
    doc.elements.push_back(bodyEl);

    std::unordered_map<std::string, StringId> ids;
    for (size_t i = 0; i < children.size(); ++i) {
        const DocNodeSpec& spec = children[i];
        IrDocElement el{};
        el.parent = spec.parent < 0 ? 1u : static_cast<uint32_t>(spec.parent) + 2u;
        el.tag = w.intern(spec.tag);
        el.id = spec.id.empty() ? kInvalidString : w.intern(spec.id);
        if (!spec.id.empty()) {
            ids[spec.id] = el.id;
        }
        std::vector<StringId> classIds;
        for (const std::string& c : spec.classes) {
            classIds.push_back(w.intern(c));
        }
        std::sort(classIds.begin(), classIds.end());
        el.classOffset = static_cast<uint32_t>(doc.classPool.size());
        for (StringId c : classIds) {
            doc.classPool.push_back(c);
        }
        el.classCount = static_cast<uint32_t>(classIds.size());
        el.inlineDeclOffset = static_cast<uint32_t>(doc.inlineDecls.size());
        for (const IrDecl& d : spec.inlineDecls) {
            doc.inlineDecls.push_back(d);
        }
        el.inlineDeclCount = static_cast<uint32_t>(spec.inlineDecls.size());
        el.firstTextChild = 0xFFFFFFFFu;
        el.scene = kInvalidScene;
        el.cameraIndex = 0xFFFFFFFFu;
        doc.elements.push_back(el);
    }
    w.setDoc(std::move(doc));

    StyleBuilder style;
    if (configureStyle) {
        configureStyle(w, style);
    }
    w.setStyle(std::move(style));

    std::vector<uint8_t> bytes = w.finish();
    Built out;
    out.ids = std::move(ids);
    Status status =
        Document::load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    return out;
}

/// A fresh StyleEngine bound to the Document's own Animator (which already has its IR's timelines
/// registered -- a separate, fresh Animator would not). These tests call engine.resolve() and
/// animator.evaluate() directly instead of Document::update(), which clears ALL dirty bits at the
/// end of the frame; these tests need kDirtyLayout/kDirtyPaint/kDirtyStyle to survive a resolve()
/// call for inspection.
struct Engine {
    Animator& animator;
    StyleEngine engine;
    explicit Engine(Document& doc) : animator(doc.animator()) { engine.init(doc.ir(), animator); }
};

}  // namespace

TEST_CASE("StyleEngine: initial values when no rule matches") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "target"}}, nullptr);
    Engine e(*b.doc);
    CHECK(e.engine.resolve(b.doc->arena(), 0.0f) > 0);

    ElementRef target = b.doc->byId("target");
    REQUIRE(target.valid());
    const ElementData& data = b.doc->arena().element(target.node);
    CHECK(data.style.display == Display::Block);
    CHECK(data.style.base[prop::Width].len.unit == LengthUnit::Auto);
    CHECK(data.style.base[prop::Color].c == Color{0, 0, 0, 1});
    CHECK(data.style.base[prop::FontSize].len.value == doctest::Approx(16.0f));
}

TEST_CASE("StyleEngine: cascade - specificity, source order, important vs inline") {
    Built b = buildDoc(
        {DocNodeSpec{.tag = "div", .id = "cascadeTarget", .classes = {"cx", "cy"}},
         DocNodeSpec{.tag = "div",
                     .id = "impTarget",
                     .classes = {"imp", "impImp"},
                     .inlineDecls = {declFloat(prop::Opacity, 0.4f)}}},
        [](IrWriter& w, StyleBuilder& style) {
            StringId classCx = w.intern("cx");
            StringId classCy = w.intern("cy");
            StringId classImp = w.intern("imp");
            StringId classImpImp = w.intern("impImp");
            StringId idCascadeTarget = w.intern("cascadeTarget");

            // Specificity: class rule (1<<8) loses to id rule (1<<16).
            addRule(style, kInvalidString, classCx, 0, 1u << 8, {declFloat(prop::Opacity, 0.2f)});
            addRule(style, idCascadeTarget, kInvalidString, 0, 1u << 16, {declFloat(prop::Opacity, 0.9f)});
            // Same specificity: source order decides (later wins).
            addRule(style, kInvalidString, classCy, 0, 1u << 8, {declFloat(styleprop::ZIndex, 3.0f)});
            addRule(style, kInvalidString, classCy, 0, 1u << 8, {declFloat(styleprop::ZIndex, 7.0f)});
            // important beats inline; inline beats a plain (non-important) class rule.
            addRule(style, kInvalidString, classImp, 0, 1u << 8, {declFloat(prop::Opacity, 0.6f)});
            addRule(style, kInvalidString, classImpImp, 0, 1u << 8,
                    {declFloat(prop::Opacity, 0.8f, /*important=*/true)});
        });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);

    ElementRef cascadeTarget = b.doc->byId("cascadeTarget");
    const ElementData& cascadeData = b.doc->arena().element(cascadeTarget.node);
    CHECK(cascadeData.style.base[prop::Opacity].f == doctest::Approx(0.9f));  // id beats class
    CHECK(cascadeData.style.zIndex == doctest::Approx(7.0f));                 // later same-specificity wins

    ElementRef impTarget = b.doc->byId("impTarget");
    const ElementData& impData = b.doc->arena().element(impTarget.node);
    CHECK(impData.style.base[prop::Opacity].f == doctest::Approx(0.8f));  // !important beats inline & class
}

TEST_CASE("StyleEngine: inheritance") {
    Built b = buildDoc(
        {DocNodeSpec{.tag = "div", .id = "parent"}, DocNodeSpec{.tag = "div", .id = "child1", .parent = 0},
         DocNodeSpec{.tag = "div", .id = "child2", .parent = 0}},
        [](IrWriter& w, StyleBuilder& style) {
            StringId idParent = w.intern("parent");
            StringId idChild2 = w.intern("child2");
            addRule(style, idParent, kInvalidString, 0, 1u << 16,
                    {declColor(prop::Color, Color{1, 0, 0, 1}), declLength(prop::Width, 50, LengthUnit::Px)});
            addRule(style, idChild2, kInvalidString, 0, 1u << 16,
                    {declColor(prop::Color, Color{0, 0, 1, 1})});
        });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);

    ElementRef child1 = b.doc->byId("child1");
    const ElementData& c1 = b.doc->arena().element(child1.node);
    CHECK(c1.style.base[prop::Color].c == Color{1, 0, 0, 1});        // inherited from parent
    CHECK(c1.style.base[prop::Width].len.unit == LengthUnit::Auto);  // width is not inherited

    ElementRef child2 = b.doc->byId("child2");
    const ElementData& c2 = b.doc->arena().element(child2.node);
    CHECK(c2.style.base[prop::Color].c == Color{0, 0, 1, 1});  // own rule overrides inheritance
}

TEST_CASE("StyleEngine: em resolution") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "emparent"},
                        DocNodeSpec{.tag = "div", .id = "emchild", .parent = 0}},
                       [](IrWriter& w, StyleBuilder& style) {
                           StringId idParent = w.intern("emparent");
                           StringId idChild = w.intern("emchild");
                           addRule(style, idParent, kInvalidString, 0, 1u << 16,
                                   {declLength(prop::FontSize, 20, LengthUnit::Px)});
                           addRule(style, idChild, kInvalidString, 0, 1u << 16,
                                   {declLength(prop::FontSize, 2, LengthUnit::Em),
                                    declLength(prop::PaddingLeft, 1, LengthUnit::Em)});
                       });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);

    ElementRef child = b.doc->byId("emchild");
    const ElementData& data = b.doc->arena().element(child.node);
    CHECK(data.style.base[prop::FontSize].len.value == doctest::Approx(40.0f));
    CHECK(data.style.base[prop::FontSize].len.unit == LengthUnit::Px);
    CHECK(data.style.base[prop::PaddingLeft].len.value == doctest::Approx(40.0f));
    CHECK(data.style.base[prop::PaddingLeft].len.unit == LengthUnit::Px);
}

TEST_CASE("StyleEngine: state pseudo-class re-resolution") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "stateTarget", .classes = {"hoverable"}}},
                       [](IrWriter& w, StyleBuilder& style) {
                           StringId classHoverable = w.intern("hoverable");
                           addRule(style, kInvalidString, classHoverable, kStateHover, 2u << 8,
                                   {declFloat(prop::Opacity, 0.5f)});
                       });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);

    ElementRef target = b.doc->byId("stateTarget");
    NodeArena& arena = b.doc->arena();
    CHECK(arena.element(target.node).style.base[prop::Opacity].f == doctest::Approx(1.0f));

    arena.element(target.node).stateBits |= kStateHover;
    arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
    e.engine.resolve(arena, 0.0f);
    CHECK(arena.element(target.node).style.base[prop::Opacity].f == doctest::Approx(0.5f));

    arena.element(target.node).stateBits &= static_cast<uint16_t>(~kStateHover);
    arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
    e.engine.resolve(arena, 0.0f);
    CHECK(arena.element(target.node).style.base[prop::Opacity].f == doctest::Approx(1.0f));
}

TEST_CASE("StyleEngine: transition registration and evaluation, not on first resolve") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "transTarget", .classes = {"transitionable"}}},
                       [](IrWriter& w, StyleBuilder& style) {
                           StringId classTransitionable = w.intern("transitionable");
                           StringId classChanged = w.intern("transChanged");
                           IrTransitionSpec spec{};
                           spec.prop = prop::Opacity;
                           spec.duration = 1.0f;
                           spec.delay = 0.0f;
                           spec.easing = linearIrEasing();
                           uint32_t transOffset = static_cast<uint32_t>(style.transitions.size());
                           style.transitions.push_back(spec);
                           addRule(style, kInvalidString, classTransitionable, 0, 1u << 8,
                                   {declListRef(styleprop::Transition, transOffset, 1)});
                           addRule(style, kInvalidString, classChanged, 0, 1u << 8,
                                   {declFloat(prop::Opacity, 0.0f)});
                       });
    Engine e(*b.doc);

    // First resolve: no transition, even though nothing has an explicit opacity yet.
    e.engine.resolve(b.doc->arena(), 0.0f);
    CHECK(e.animator.transitionCount() == 0);

    ElementRef target = b.doc->byId("transTarget");
    CHECK(target.addClass("transChanged"));
    e.engine.resolve(b.doc->arena(), 0.0f);
    CHECK(e.animator.transitionCount() == 1);

    e.animator.evaluate(0.5f, b.doc->arena());
    Value v = getProperty(b.doc->arena(), target.node, prop::Opacity);
    CHECK(v.f == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("StyleEngine: CSS animation play/stop, pause, alternate") {
    TimelineId opacityFade = kInvalidTimeline;
    Built b = buildDoc(
        {DocNodeSpec{.tag = "div", .id = "animTarget"}, DocNodeSpec{.tag = "div", .id = "pausedTarget"},
         DocNodeSpec{.tag = "div", .id = "altTarget"}},
        [&](IrWriter& w, StyleBuilder& style) {
            TimelineBuilder tb;
            tb.duration = 1.0f;
            IrTrack track{};
            track.target = kSelfNode;
            track.prop = prop::Opacity;
            track.type = static_cast<uint8_t>(ValueType::Float);
            track.kfOffset = 0;
            track.kfCount = 2;
            tb.tracks.push_back(track);
            IrKeyframe kf0{};
            kf0.time = 0.0f;
            kf0.easing = linearIrEasing();
            irSetValueF32(kf0.value, 0, 0.0f);
            IrKeyframe kf1{};
            kf1.time = 1.0f;
            kf1.easing = linearIrEasing();
            irSetValueF32(kf1.value, 0, 1.0f);
            tb.keyframes.push_back(kf0);
            tb.keyframes.push_back(kf1);
            opacityFade = w.addTimeline(std::move(tb));

            auto addAnimRule = [&](StringId classId, uint16_t iterations, uint8_t direction,
                                   uint8_t playState) {
                IrAnimationSpec spec{};
                spec.timeline = opacityFade;
                spec.duration = 1.0f;
                spec.delay = 0.0f;
                spec.easing = linearIrEasing();
                spec.iterations = iterations;
                spec.direction = direction;
                spec.fill = static_cast<uint8_t>(FillMode::Forwards);
                spec.playState = playState;
                uint32_t off = static_cast<uint32_t>(style.animations.size());
                style.animations.push_back(spec);
                addRule(style, kInvalidString, classId, 0, 1u << 8,
                        {declListRef(styleprop::Animation, off, 1)});
            };
            addAnimRule(w.intern("anim1"), 1, 0, 0);
            addAnimRule(w.intern("animPaused"), 1, 0, 1);
            addAnimRule(w.intern("animAlt"), 0, 2, 0);
        });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);

    SUBCASE("play on addClass, stop on removeClass") {
        ElementRef target = b.doc->byId("animTarget");
        CHECK(target.addClass("anim1"));
        e.engine.resolve(b.doc->arena(), 0.0f);
        CHECK(e.animator.activeInstanceCount() == 1);
        CHECK(b.doc->arena().element(target.node).cssAnimations.size() == 1);

        CHECK(target.removeClass("anim1"));
        e.engine.resolve(b.doc->arena(), 0.0f);
        CHECK(e.animator.activeInstanceCount() == 0);
        CHECK(b.doc->arena().element(target.node).cssAnimations.empty());
    }

    SUBCASE("paused: does not advance") {
        ElementRef target = b.doc->byId("pausedTarget");
        target.addClass("animPaused");
        e.engine.resolve(b.doc->arena(), 0.0f);
        e.animator.evaluate(0.0f, b.doc->arena());
        float v0 = getProperty(b.doc->arena(), target.node, prop::Opacity).f;
        e.animator.evaluate(2.0f, b.doc->arena());
        float v1 = getProperty(b.doc->arena(), target.node, prop::Opacity).f;
        CHECK(v0 == doctest::Approx(v1));
    }

    SUBCASE("alternate infinite: reverses after the first cycle") {
        ElementRef target = b.doc->byId("altTarget");
        target.addClass("animAlt");
        e.engine.resolve(b.doc->arena(), 0.0f);
        e.animator.evaluate(1.0f, b.doc->arena());
        float atPeak = getProperty(b.doc->arena(), target.node, prop::Opacity).f;
        e.animator.evaluate(1.5f, b.doc->arena());
        float afterPeak = getProperty(b.doc->arena(), target.node, prop::Opacity).f;
        CHECK(afterPeak < atPeak);
    }
}

TEST_CASE("StyleEngine: non-animatable field changes mark kDirtyLayout/kDirtyPaint") {
    Built b = buildDoc(
        {DocNodeSpec{.tag = "div", .id = "layoutTarget"}, DocNodeSpec{.tag = "div", .id = "paintTarget"}},
        [](IrWriter& w, StyleBuilder& style) {
            addRule(style, kInvalidString, w.intern("flexify"), 0, 1u << 8,
                    {declEnum(styleprop::Display, 1)});  // flex
            addRule(style, kInvalidString, w.intern("hideMe"), 0, 1u << 8,
                    {declEnum(styleprop::Visibility, 1)});  // hidden
        });
    Engine e(*b.doc);
    e.engine.resolve(b.doc->arena(), 0.0f);
    NodeArena& arena = b.doc->arena();

    ElementRef layoutTarget = b.doc->byId("layoutTarget");
    layoutTarget.addClass("flexify");
    e.engine.resolve(arena, 0.0f);
    CHECK((arena.dirty(layoutTarget.node) & static_cast<uint8_t>(kDirtyLayout)) != 0);

    ElementRef paintTarget = b.doc->byId("paintTarget");
    paintTarget.addClass("hideMe");
    e.engine.resolve(arena, 0.0f);
    CHECK((arena.dirty(paintTarget.node) & static_cast<uint8_t>(kDirtyPaint)) != 0);
}

TEST_CASE("StyleEngine: dirty propagation - ancestors and descendants only") {
    // body > A > [B, C]; B > D.
    Built b =
        buildDoc({DocNodeSpec{.tag = "div", .id = "A"}, DocNodeSpec{.tag = "div", .id = "B", .parent = 0},
                  DocNodeSpec{.tag = "div", .id = "C", .parent = 0},
                  DocNodeSpec{.tag = "div", .id = "D", .parent = 1}},
                 nullptr);
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();

    uint32_t total = e.engine.resolve(arena, 0.0f);
    CHECK(total == arena.aliveCount());         // everything starts dirty after load
    CHECK(e.engine.resolve(arena, 0.0f) == 0);  // nothing dirty now

    ElementRef bRef = b.doc->byId("B");
    CHECK(bRef.setState(kStateDisabled, true));
    // Dirty: xt, body, A, B (self+ancestors) + D (descendant of B). C is untouched.
    CHECK(e.engine.resolve(arena, 0.0f) == 5);
    CHECK(e.engine.resolve(arena, 0.0f) == 0);
}

TEST_CASE("StyleEngine: resolve does not depend on NodeId order (freed-slot reuse)") {
    // NodeArena reuses freed slots (see node_arena.cpp NodeArena::create/allocateData), so a child
    // created after its would-be parent's slot was freed and reused can end up with a LOWER NodeId
    // than its parent. resolve() must still process the parent before the child (a real pre-order
    // walk from the tree root via NodeArena::traversePreorder), not rely on NodeId ordering.
    IrWriter w;
    StringId parentId = w.intern("parent");
    StyleBuilder style;
    addRule(style, parentId, kInvalidString, 0, 1u << 16, {declColor(prop::Color, Color{1, 0, 0, 1})});
    w.setStyle(std::move(style));
    w.setDoc(DocBuilder{});  // Not used -- the tree below is built directly on a bare NodeArena.
    std::vector<uint8_t> bytes = w.finish();
    Result<IrView> opened = IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{true});
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();

    NodeArena arena;
    NodeId placeholder = arena.create(NodeKind::Element);
    arena.destroy(placeholder);                       // Frees the slot.
    NodeId child = arena.create(NodeKind::Element);   // Reuses the freed slot -> low NodeId.
    NodeId parent = arena.create(NodeKind::Element);  // Fresh slot -> higher NodeId.
    REQUIRE(child < parent);
    arena.element(parent).id = parentId;
    arena.appendChild(parent, child);
    arena.markDirty(child, static_cast<uint8_t>(kDirtyStyle));  // Propagates to `parent` too.

    Animator animator;
    StyleEngine engine;
    engine.init(ir, animator);
    CHECK(engine.resolve(arena, 0.0f) == 2);

    CHECK(arena.element(parent).style.base[prop::Color].c == Color{1, 0, 0, 1});
    CHECK(arena.element(child).style.base[prop::Color].c == Color{1, 0, 0, 1});  // inherited
}

// §19.3, docs/tasks/T-43c.md: Element is3D runtime derivation. `.rot` sets PropId 18 (RotationX)
// non-zero via a compiled inline-decl-style rule (declFloat -- mini_css has no 3D function syntax
// yet, T-47), so toggling the class exercises StyleEngine::resolve's "7. Commit" is3D rule
// without needing CSS transform-function parsing.
TEST_CASE("StyleEngine: Element is3D derives from base[16..22] each resolve") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "target"}}, [](IrWriter& w, StyleBuilder& style) {
        addRule(style, kInvalidString, w.intern("rot"), 0, 1u << 8, {declFloat(prop::RotationX, 45.0f)});
    });
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();
    e.engine.resolve(arena, 0.0f);

    ElementRef target = b.doc->byId("target");
    REQUIRE(target.valid());
    CHECK(arena.common(target.node).is3D == 0);

    target.addClass("rot");
    e.engine.resolve(arena, 0.0f);
    CHECK(arena.common(target.node).is3D == 1);
    CHECK((arena.dirty(target.node) & static_cast<uint8_t>(kDirtyTree)) != 0);

    target.removeClass("rot");
    e.engine.resolve(arena, 0.0f);
    CHECK(arena.common(target.node).is3D == 0);
}

// §19.4/19.10, docs/tasks/T-47a.md: perspective/perspective-origin/backface-visibility cascade
// into base[106..108] and ComputedStyle::backfaceHidden.
TEST_CASE("StyleEngine: perspective/perspective-origin/backface-visibility cascade") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "a"}}, [](IrWriter& w, StyleBuilder& style) {
        addRule(style, w.intern("a"), kInvalidString, 0, 1u << 16,
                {declLength(prop::Perspective, 500, LengthUnit::Px),
                 declLength(prop::PerspectiveOriginX, 10, LengthUnit::Px),
                 declLength(prop::PerspectiveOriginY, 20, LengthUnit::Px),
                 declEnum(style_decode::styleprop::BackfaceVisibility, 1)});
    });
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();
    e.engine.resolve(arena, 0.0f);

    ElementRef a = b.doc->byId("a");
    REQUIRE(a.valid());
    const ElementData& data = arena.element(a.node);
    CHECK(data.style.base[prop::Perspective].len.value == doctest::Approx(500));
    CHECK(data.style.base[prop::Perspective].len.unit == LengthUnit::Px);
    CHECK(data.style.base[prop::PerspectiveOriginX].len.value == doctest::Approx(10));
    CHECK(data.style.base[prop::PerspectiveOriginY].len.value == doctest::Approx(20));
    CHECK(data.style.backfaceHidden == 1);
    CHECK(arena.common(a.node).backfaceHidden == 1);
}

// §19.3/19.10, docs/tasks/T-47a.md: `:hover { transform: rotateY(45deg) }` -> is3D derives to 1
// once the hover state pseudo-class matches (same is3D-derivation mechanism as the RotationX test
// above, exercised through RotationY this time).
TEST_CASE("StyleEngine: :hover rotateY(45deg) makes the element is3D") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "target"}}, [](IrWriter&, StyleBuilder& style) {
        addRule(style, kInvalidString, kInvalidString, kStateHover, 1u << 8,
                {declFloat(prop::RotationY, 45.0f)});
    });
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();
    e.engine.resolve(arena, 0.0f);

    ElementRef target = b.doc->byId("target");
    REQUIRE(target.valid());
    CHECK(arena.common(target.node).is3D == 0);

    arena.element(target.node).stateBits |= kStateHover;
    arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
    e.engine.resolve(arena, 0.0f);
    CHECK(arena.common(target.node).is3D == 1);
}

// docs/design/18-effects.md §18.4, docs/tasks/T-51b.md: `effect` cascade (whole-sequence
// replace), same-kind-sequence -> per-parameter transition via `transition: effect <time>`.
TEST_CASE("StyleEngine: effect cascade - declaration and same-kind transition") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "a"}}, [](IrWriter&, StyleBuilder& style) {
        IrEffect blur2{};
        blur2.kind = static_cast<uint8_t>(EffectKind::Blur);
        blur2.enabled = 1;
        blur2.shader = kInvalidAsset;
        blur2.params[0] = 2.0f;
        blur2.params[1] = 2.0f;
        uint32_t blur2Idx = IrWriter::addStyleEffect(style, blur2);

        IrEffect blur10 = blur2;
        blur10.params[0] = 10.0f;
        blur10.params[1] = 10.0f;
        uint32_t blur10Idx = IrWriter::addStyleEffect(style, blur10);

        IrTransitionSpec spec{};
        spec.prop = styleprop::Effect;
        spec.duration = 0.2f;
        spec.delay = 0.0f;
        spec.easing = linearIrEasing();
        uint32_t transOffset = static_cast<uint32_t>(style.transitions.size());
        style.transitions.push_back(spec);

        addRule(style, kInvalidString, kInvalidString, 0, 1u << 8,
                {declListRef(styleprop::Effect, blur2Idx, 1),
                 declListRef(styleprop::Transition, transOffset, 1)});
        addRule(style, kInvalidString, kInvalidString, kStateHover, 2u << 8,
                {declListRef(styleprop::Effect, blur10Idx, 1)});
    });
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();
    e.engine.resolve(arena, 0.0f);

    ElementRef target = b.doc->byId("a");
    REQUIRE(target.valid());
    CHECK(arena.common(target.node).effectCount == 1);
    CHECK(arena.effects()[arena.common(target.node).effectOffset].params[0] == doctest::Approx(2.0f));

    arena.element(target.node).stateBits |= kStateHover;
    arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
    e.engine.resolve(arena, 0.0f);
    CHECK(e.animator.hasTransition(target.node, effectParam(0, 1)));

    e.animator.evaluate(0.1f, arena);
    Value v = getProperty(arena, target.node, effectParam(0, 1));
    CHECK(v.f == doctest::Approx(6.0f).epsilon(0.05));
}

// 18.4/T-51b: a kind-changing sequence replaces outright, without a transition, even though a
// `transition: effect ...` is declared.
TEST_CASE("StyleEngine: effect cascade - kind change replaces without transition") {
    Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "a"}}, [](IrWriter&, StyleBuilder& style) {
        IrEffect blur2{};
        blur2.kind = static_cast<uint8_t>(EffectKind::Blur);
        blur2.enabled = 1;
        blur2.shader = kInvalidAsset;
        blur2.params[0] = 2.0f;
        blur2.params[1] = 2.0f;
        uint32_t blur2Idx = IrWriter::addStyleEffect(style, blur2);

        IrEffect shadow{};
        shadow.kind = static_cast<uint8_t>(EffectKind::DropShadow);
        shadow.enabled = 1;
        shadow.shader = kInvalidAsset;
        shadow.params[0] = 1.0f;
        shadow.params[1] = 2.0f;
        shadow.params[2] = 3.0f;
        shadow.params[4] = 1.0f;
        shadow.colors[0] = Color{1, 0, 0, 1};
        uint32_t shadowIdx = IrWriter::addStyleEffect(style, shadow);

        IrTransitionSpec spec{};
        spec.prop = styleprop::Effect;
        spec.duration = 0.2f;
        spec.delay = 0.0f;
        spec.easing = linearIrEasing();
        uint32_t transOffset = static_cast<uint32_t>(style.transitions.size());
        style.transitions.push_back(spec);

        addRule(style, kInvalidString, kInvalidString, 0, 1u << 8,
                {declListRef(styleprop::Effect, blur2Idx, 1),
                 declListRef(styleprop::Transition, transOffset, 1)});
        addRule(style, kInvalidString, kInvalidString, kStateHover, 2u << 8,
                {declListRef(styleprop::Effect, shadowIdx, 1)});
    });
    Engine e(*b.doc);
    NodeArena& arena = b.doc->arena();
    e.engine.resolve(arena, 0.0f);

    ElementRef target = b.doc->byId("a");
    REQUIRE(target.valid());

    arena.element(target.node).stateBits |= kStateHover;
    arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
    e.engine.resolve(arena, 0.0f);

    CHECK(arena.common(target.node).effectCount == 1);
    CHECK(arena.effects()[arena.common(target.node).effectOffset].kind == EffectKind::DropShadow);
    CHECK_FALSE(e.animator.hasTransition(target.node, effectParam(0, 1)));
}

// T-208 (18 §18.13): two custom() effects with different shader assets are different sequences --
// replaced outright like a kind change; the same shader still transitions its parameters.
TEST_CASE("StyleEngine: effect cascade - custom effects compare their shader asset") {
    for (const AssetId hoverShader : {AssetId{1}, AssetId{0}}) {
        Built b = buildDoc({DocNodeSpec{.tag = "div", .id = "a"}}, [&](IrWriter& w, StyleBuilder& style) {
            // Two `.xtfx` assets (ASST kind 2) for the effects to name.
            for (const char* name : {"a.xtfx", "b.xtfx"}) {
                AssetBuilder asset;
                asset.name = w.intern(name);
                asset.kind = 2;
                w.addAsset(asset);
            }
            IrEffect ripple{};
            ripple.kind = static_cast<uint8_t>(EffectKind::Custom);
            ripple.enabled = 1;
            ripple.shader = 0;
            ripple.params[0] = 2.0f;
            uint32_t rippleIdx = IrWriter::addStyleEffect(style, ripple);

            IrEffect other = ripple;
            other.shader = hoverShader;
            other.params[0] = 10.0f;
            uint32_t otherIdx = IrWriter::addStyleEffect(style, other);

            IrTransitionSpec spec{};
            spec.prop = styleprop::Effect;
            spec.duration = 0.2f;
            spec.delay = 0.0f;
            spec.easing = linearIrEasing();
            uint32_t transOffset = static_cast<uint32_t>(style.transitions.size());
            style.transitions.push_back(spec);

            addRule(style, kInvalidString, kInvalidString, 0, 1u << 8,
                    {declListRef(styleprop::Effect, rippleIdx, 1),
                     declListRef(styleprop::Transition, transOffset, 1)});
            addRule(style, kInvalidString, kInvalidString, kStateHover, 2u << 8,
                    {declListRef(styleprop::Effect, otherIdx, 1)});
        });
        Engine e(*b.doc);
        NodeArena& arena = b.doc->arena();
        e.engine.resolve(arena, 0.0f);
        ElementRef target = b.doc->byId("a");
        REQUIRE(target.valid());

        arena.element(target.node).stateBits |= kStateHover;
        arena.markDirty(target.node, static_cast<uint8_t>(kDirtyStyle));
        e.engine.resolve(arena, 0.0f);

        const EffectInstance& slot = arena.effects()[arena.common(target.node).effectOffset];
        CHECK(slot.shader == hoverShader);
        if (hoverShader != 0) {
            // A different custom effect: the new stack at once, no parameter transition.
            CHECK_FALSE(e.animator.hasTransition(target.node, effectParam(0, 1)));
            CHECK(getProperty(arena, target.node, effectParam(0, 1)).f == doctest::Approx(10.0f));
        } else {
            CHECK(e.animator.hasTransition(target.node, effectParam(0, 1)));
        }
    }
}

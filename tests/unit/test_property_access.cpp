#include <cstring>

#include "doctest.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xt/value.h"

using namespace xt;

namespace {

struct ExpectedProp {
    PropId id;
    const char* name;
    ValueType type;
    uint8_t dirty;
};

// clang-format off
constexpr ExpectedProp kExpected[] = {
    {prop::Opacity, "Opacity", ValueType::Float, kDirtyPaint},
    {prop::AnchorX, "AnchorX", ValueType::Float, kDirtyTransform},
    {prop::AnchorY, "AnchorY", ValueType::Float, kDirtyTransform},
    {prop::TranslateX, "TranslateX", ValueType::Float, kDirtyTransform},
    {prop::TranslateY, "TranslateY", ValueType::Float, kDirtyTransform},
    {prop::ScaleX, "ScaleX", ValueType::Float, kDirtyTransform},
    {prop::ScaleY, "ScaleY", ValueType::Float, kDirtyTransform},
    {prop::Rotation, "Rotation", ValueType::Float, kDirtyTransform},
    {prop::SkewX, "SkewX", ValueType::Float, kDirtyTransform},
    {prop::SkewY, "SkewY", ValueType::Float, kDirtyTransform},
    {prop::Anchor, "Anchor", ValueType::Vec2, kDirtyTransform},
    {prop::Translate, "Translate", ValueType::Vec2, kDirtyTransform},
    {prop::Scale, "Scale", ValueType::Vec2, kDirtyTransform},
    {prop::Visible, "Visible", ValueType::Enum, kDirtyTransform},

    {prop::TranslateZ, "TranslateZ", ValueType::Float, kDirtyTransform},
    {prop::AnchorZ, "AnchorZ", ValueType::Float, kDirtyTransform},
    {prop::RotationX, "RotationX", ValueType::Float, kDirtyTransform},
    {prop::RotationY, "RotationY", ValueType::Float, kDirtyTransform},
    {prop::Translate3, "Translate3", ValueType::Vec3, kDirtyTransform},
    {prop::Anchor3, "Anchor3", ValueType::Vec3, kDirtyTransform},
    {prop::Orientation, "Orientation", ValueType::Vec3, kDirtyTransform},
    {prop::CameraZoom, "CameraZoom", ValueType::Float, kDirtyTransform},
    {prop::CameraTarget, "CameraTarget", ValueType::Vec3, kDirtyTransform},
    {prop::CameraNear, "CameraNear", ValueType::Float, kDirtyTransform},
    {prop::CameraFar, "CameraFar", ValueType::Float, kDirtyTransform},

    {prop::PathData, "PathData", ValueType::Path, kDirtyPaint},
    {prop::FillColor, "FillColor", ValueType::Color, kDirtyPaint},
    {prop::FillOpacity, "FillOpacity", ValueType::Float, kDirtyPaint},
    {prop::StrokeColor, "StrokeColor", ValueType::Color, kDirtyPaint},
    {prop::StrokeOpacity, "StrokeOpacity", ValueType::Float, kDirtyPaint},
    {prop::StrokeWidth, "StrokeWidth", ValueType::Float, kDirtyPaint},
    {prop::TrimStart, "TrimStart", ValueType::Float, kDirtyPaint},
    {prop::TrimEnd, "TrimEnd", ValueType::Float, kDirtyPaint},
    {prop::TrimOffset, "TrimOffset", ValueType::Float, kDirtyPaint},
    {prop::FillGradientP0, "FillGradientP0", ValueType::Vec2, kDirtyPaint},
    {prop::FillGradientP1, "FillGradientP1", ValueType::Vec2, kDirtyPaint},
    {prop::FillGradientStops, "FillGradientStops", ValueType::Stops, kDirtyPaint},
    {prop::StrokeGradientP0, "StrokeGradientP0", ValueType::Vec2, kDirtyPaint},
    {prop::StrokeGradientP1, "StrokeGradientP1", ValueType::Vec2, kDirtyPaint},
    {prop::StrokeGradientStops, "StrokeGradientStops", ValueType::Stops, kDirtyPaint},
    {prop::DashOffset, "DashOffset", ValueType::Float, kDirtyPaint},
    {prop::DashArray, "DashArray", ValueType::FloatArray, kDirtyPaint},
    {prop::MiterLimit, "MiterLimit", ValueType::Float, kDirtyPaint},
    {prop::StrokeTaperStart, "StrokeTaperStart", ValueType::Float, kDirtyPaint},
    {prop::StrokeTaperEnd, "StrokeTaperEnd", ValueType::Float, kDirtyPaint},

    {prop::Width, "Width", ValueType::Length, kDirtyLayout},
    {prop::Height, "Height", ValueType::Length, kDirtyLayout},
    {prop::MinWidth, "MinWidth", ValueType::Length, kDirtyLayout},
    {prop::MaxWidth, "MaxWidth", ValueType::Length, kDirtyLayout},
    {prop::MinHeight, "MinHeight", ValueType::Length, kDirtyLayout},
    {prop::MaxHeight, "MaxHeight", ValueType::Length, kDirtyLayout},
    {prop::MarginTop, "MarginTop", ValueType::Length, kDirtyLayout},
    {prop::MarginRight, "MarginRight", ValueType::Length, kDirtyLayout},
    {prop::MarginBottom, "MarginBottom", ValueType::Length, kDirtyLayout},
    {prop::MarginLeft, "MarginLeft", ValueType::Length, kDirtyLayout},
    {prop::PaddingTop, "PaddingTop", ValueType::Length, kDirtyLayout},
    {prop::PaddingRight, "PaddingRight", ValueType::Length, kDirtyLayout},
    {prop::PaddingBottom, "PaddingBottom", ValueType::Length, kDirtyLayout},
    {prop::PaddingLeft, "PaddingLeft", ValueType::Length, kDirtyLayout},
    {prop::BorderTopWidth, "BorderTopWidth", ValueType::Length, kDirtyLayout},
    {prop::BorderRightWidth, "BorderRightWidth", ValueType::Length, kDirtyLayout},
    {prop::BorderBottomWidth, "BorderBottomWidth", ValueType::Length, kDirtyLayout},
    {prop::BorderLeftWidth, "BorderLeftWidth", ValueType::Length, kDirtyLayout},
    {prop::Left, "Left", ValueType::Length, kDirtyLayout},
    {prop::Top, "Top", ValueType::Length, kDirtyLayout},
    {prop::Right, "Right", ValueType::Length, kDirtyLayout},
    {prop::Bottom, "Bottom", ValueType::Length, kDirtyLayout},
    {prop::FlexGrow, "FlexGrow", ValueType::Float, kDirtyLayout},
    {prop::FlexShrink, "FlexShrink", ValueType::Float, kDirtyLayout},
    {prop::FlexBasis, "FlexBasis", ValueType::Length, kDirtyLayout},
    {prop::RowGap, "RowGap", ValueType::Length, kDirtyLayout},
    {prop::ColumnGap, "ColumnGap", ValueType::Length, kDirtyLayout},
    {prop::FontSize, "FontSize", ValueType::Length, kDirtyLayout},
    {prop::LineHeight, "LineHeight", ValueType::Length, kDirtyLayout},
    {prop::LetterSpacing, "LetterSpacing", ValueType::Length, kDirtyLayout},

    {prop::BackgroundColor, "BackgroundColor", ValueType::Color, kDirtyPaint},
    {prop::BorderColor, "BorderColor", ValueType::Color, kDirtyPaint},
    {prop::BorderRadiusTL, "BorderRadiusTL", ValueType::Length, kDirtyPaint},
    {prop::BorderRadiusTR, "BorderRadiusTR", ValueType::Length, kDirtyPaint},
    {prop::BorderRadiusBR, "BorderRadiusBR", ValueType::Length, kDirtyPaint},
    {prop::BorderRadiusBL, "BorderRadiusBL", ValueType::Length, kDirtyPaint},
    {prop::Color, "Color", ValueType::Color, kDirtyPaint},
    {prop::ImageOpacity, "ImageOpacity", ValueType::Float, kDirtyPaint},
    // T-32: ScrollX/Y feed layout_engine.cpp's applyTransforms, so their dirty bit is L, not P
    // (docs/tasks/T-32.md "把 ScrollX/ScrollY 的髒位改為 L").
    {prop::ScrollX, "ScrollX", ValueType::Float, kDirtyLayout},
    {prop::ScrollY, "ScrollY", ValueType::Float, kDirtyLayout},
    // 19.4/19.10 (T-47a): a perspective change can flip the element's 3D-space-root status, so it
    // dirties the whole subtree's world transforms too (kDirtyTree), not just this node's.
    {prop::Perspective, "Perspective", ValueType::Length,
     static_cast<uint8_t>(kDirtyTransform | kDirtyTree)},
    {prop::PerspectiveOriginX, "PerspectiveOriginX", ValueType::Length,
     static_cast<uint8_t>(kDirtyTransform | kDirtyTree)},
    {prop::PerspectiveOriginY, "PerspectiveOriginY", ValueType::Length,
     static_cast<uint8_t>(kDirtyTransform | kDirtyTree)},
    // 07.8 (T-178): typewriter reveal, paint-only.
    {prop::TextReveal, "TextReveal", ValueType::Float, kDirtyPaint},
    // T-202 (03 §3.12.5): the particle fields, paint-only.
    {prop::ParticleRate, "ParticleRate", ValueType::Float, kDirtyPaint},
    {prop::ParticleBurst, "ParticleBurst", ValueType::Float, kDirtyPaint},
    {prop::ParticleLifetimeMin, "ParticleLifetimeMin", ValueType::Float, kDirtyPaint},
    {prop::ParticleLifetimeMax, "ParticleLifetimeMax", ValueType::Float, kDirtyPaint},
    {prop::ParticleSpeedMin, "ParticleSpeedMin", ValueType::Float, kDirtyPaint},
    {prop::ParticleSpeedMax, "ParticleSpeedMax", ValueType::Float, kDirtyPaint},
    {prop::ParticleDirection, "ParticleDirection", ValueType::Float, kDirtyPaint},
    {prop::ParticleSpread, "ParticleSpread", ValueType::Float, kDirtyPaint},
    {prop::ParticleGravity, "ParticleGravity", ValueType::Float, kDirtyPaint},
    {prop::ParticleDrag, "ParticleDrag", ValueType::Float, kDirtyPaint},
    {prop::ParticleSizeStart, "ParticleSizeStart", ValueType::Float, kDirtyPaint},
    {prop::ParticleSizeEnd, "ParticleSizeEnd", ValueType::Float, kDirtyPaint},
    {prop::ParticleColor0, "ParticleColor0", ValueType::Color, kDirtyPaint},
    {prop::ParticleColor1, "ParticleColor1", ValueType::Color, kDirtyPaint},
    {prop::ParticleColor2, "ParticleColor2", ValueType::Color, kDirtyPaint},
    {prop::ParticleColor3, "ParticleColor3", ValueType::Color, kDirtyPaint},
    {prop::ParticleRotationMin, "ParticleRotationMin", ValueType::Float, kDirtyPaint},
    {prop::ParticleRotationMax, "ParticleRotationMax", ValueType::Float, kDirtyPaint},
    {prop::ParticleMax, "ParticleMax", ValueType::Float, kDirtyPaint},
    {prop::OutlineWidth, "OutlineWidth", ValueType::Length, kDirtyPaint},  // T-325
    {prop::OutlineColor, "OutlineColor", ValueType::Color, kDirtyPaint},
    {prop::OutlineOffset, "OutlineOffset", ValueType::Length, kDirtyPaint},
    {prop::BackgroundSizeX, "BackgroundSizeX", ValueType::Length, kDirtyPaint},  // T-327
    {prop::BackgroundSizeY, "BackgroundSizeY", ValueType::Length, kDirtyPaint},
    {prop::BackgroundPositionX, "BackgroundPositionX", ValueType::Length, kDirtyPaint},
    {prop::BackgroundPositionY, "BackgroundPositionY", ValueType::Length, kDirtyPaint},
    {prop::AspectRatio, "AspectRatio", ValueType::Float, kDirtyLayout},  // T-328
    {prop::BorderTopColor, "BorderTopColor", ValueType::Color, kDirtyPaint},  // T-333
    {prop::BorderRightColor, "BorderRightColor", ValueType::Color, kDirtyPaint},
    {prop::BorderBottomColor, "BorderBottomColor", ValueType::Color, kDirtyPaint},
    {prop::BorderLeftColor, "BorderLeftColor", ValueType::Color, kDirtyPaint},
    {prop::Order, "Order", ValueType::Float, kDirtyLayout},  // T-336
};
// clang-format on

}  // namespace

TEST_CASE("propertyType/Applies/DirtyBits/Name/ByName over every defined PropId") {
    for (const ExpectedProp& e : kExpected) {
        CAPTURE(e.name);
        CHECK(propertyDefined(e.id));
        CHECK(propertyType(e.id) == e.type);
        CHECK(propertyDirtyBits(e.id) == e.dirty);
        CHECK(std::strcmp(propertyName(e.id), e.name) == 0);
        CHECK(propertyByName(e.name) == e.id);
    }
}

TEST_CASE("propertyDefined: undefined PropIds") {
    // 106-108 (Perspective/PerspectiveOriginX/Y) are now defined (19.4/19.10, T-47a), 109
    // (TextReveal) since T-178, 110-128 (particles) since T-202, 129-131 (outline) since T-325,
    // 132-133 (text decoration) since T-324, 134-137 (background size / position) since T-327, 138
    // (aspect-ratio) since T-328, 139-142 (per-side border colors) since T-333, 143 (order) since
    // T-336; 144 is the new one-past-the-end PropId (kElementPropCount).
    // T-225: 208-231 (modifiers) and 232-245 (shapes) are defined; 160-207 are not. T-243:
    // 246-255 (rig) are defined; 256+ are not.
    // T-233: 50-51 (stroke taper) are defined; 52-63 are not.
    PropId undefined[] = {15, 23, 28, 31, 52, 60, 63, 94, 95, 144, 159, 160, 183, 192, 205, 207, 256, 300};
    for (PropId id : undefined) {
        CAPTURE(id);
        CHECK_FALSE(propertyDefined(id));
        CHECK(std::strcmp(propertyName(id), "?") == 0);
    }
    CHECK(propertyByName("NotAProperty") == prop::kInvalidProp);
}

TEST_CASE("propertyApplies: general props apply to every non-Free kind") {
    CHECK(propertyApplies(NodeKind::Group, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Path, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Image, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Text, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Element, prop::Opacity));
    CHECK(propertyApplies(NodeKind::Mask, prop::Opacity));
    CHECK_FALSE(propertyApplies(NodeKind::Free, prop::Opacity));
}

TEST_CASE("propertyApplies: Path/Element exclusivity") {
    CHECK(propertyApplies(NodeKind::Path, prop::FillColor));
    CHECK_FALSE(propertyApplies(NodeKind::Element, prop::FillColor));
    CHECK(propertyApplies(NodeKind::Element, prop::Width));
    CHECK_FALSE(propertyApplies(NodeKind::Path, prop::Width));
}

TEST_CASE("propertyApplies: 3D props apply to every non-Free kind (including Camera)") {
    CHECK(propertyApplies(NodeKind::Group, prop::TranslateZ));
    CHECK(propertyApplies(NodeKind::Path, prop::Orientation));
    CHECK(propertyApplies(NodeKind::Image, prop::RotationX));
    CHECK(propertyApplies(NodeKind::Text, prop::Anchor3));
    CHECK(propertyApplies(NodeKind::Element, prop::Translate3));
    CHECK(propertyApplies(NodeKind::Mask, prop::RotationY));
    CHECK(propertyApplies(NodeKind::Camera, prop::Translate3));
    CHECK_FALSE(propertyApplies(NodeKind::Free, prop::TranslateZ));
}

TEST_CASE("propertyApplies: Camera props only apply to Camera") {
    CHECK(propertyApplies(NodeKind::Camera, prop::CameraZoom));
    CHECK_FALSE(propertyApplies(NodeKind::Group, prop::CameraZoom));
    CHECK_FALSE(propertyApplies(NodeKind::Path, prop::CameraZoom));
    CHECK_FALSE(propertyApplies(NodeKind::Element, prop::CameraTarget));
}

TEST_CASE("propertyDefined: PropId 23 (reserved gap) is undefined") { CHECK_FALSE(propertyDefined(23)); }

TEST_CASE("3D props: set/get round trip, Translate3/Anchor3 stay in sync with components") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);
    arena.clearAllDirty();

    setProperty(arena, n, prop::RotationX, Value::ofFloat(30));
    CHECK(getProperty(arena, n, prop::RotationX).f == doctest::Approx(30));
    CHECK(arena.common(n).rotationX == doctest::Approx(30));
    CHECK((arena.dirty(n) & kDirtyTransform) != 0);

    setProperty(arena, n, prop::Orientation, Value::ofVec3(Vec3{1, 2, 3}));
    Value orient = getProperty(arena, n, prop::Orientation);
    CHECK(orient.v3.x == doctest::Approx(1));
    CHECK(orient.v3.y == doctest::Approx(2));
    CHECK(orient.v3.z == doctest::Approx(3));

    // Setting Translate3 must keep TranslateX/TranslateY/TranslateZ (PropId 3/4/16) in sync.
    setProperty(arena, n, prop::Translate3, Value::ofVec3(Vec3{5, 6, 7}));
    CHECK(getProperty(arena, n, prop::TranslateX).f == doctest::Approx(5));
    CHECK(getProperty(arena, n, prop::TranslateY).f == doctest::Approx(6));
    CHECK(getProperty(arena, n, prop::TranslateZ).f == doctest::Approx(7));
    Value t3 = getProperty(arena, n, prop::Translate3);
    CHECK(t3.v3.x == doctest::Approx(5));
    CHECK(t3.v3.y == doctest::Approx(6));
    CHECK(t3.v3.z == doctest::Approx(7));

    // Setting the scalar component keeps the composed Vec3 in sync too.
    setProperty(arena, n, prop::TranslateZ, Value::ofFloat(9));
    Value t3b = getProperty(arena, n, prop::Translate3);
    CHECK(t3b.v3.x == doctest::Approx(5));
    CHECK(t3b.v3.y == doctest::Approx(6));
    CHECK(t3b.v3.z == doctest::Approx(9));

    setProperty(arena, n, prop::Anchor3, Value::ofVec3(Vec3{11, 12, 13}));
    CHECK(getProperty(arena, n, prop::AnchorX).f == doctest::Approx(11));
    CHECK(getProperty(arena, n, prop::AnchorY).f == doctest::Approx(12));
    CHECK(getProperty(arena, n, prop::AnchorZ).f == doctest::Approx(13));

    // Setting 16-22 must not touch is3D (03.9: only StyleEngine/Animator derive it for Element;
    // scene nodes are static -- see docs/design/19-3d-space.md §19.3).
    CHECK(arena.common(n).is3D == 0);
}

TEST_CASE("Camera node: CameraZoom/Target/Near/Far set/get") {
    NodeArena arena;
    NodeId cam = arena.create(NodeKind::Camera);
    arena.clearAllDirty();

    setProperty(arena, cam, prop::CameraZoom, Value::ofFloat(500));
    CHECK(getProperty(arena, cam, prop::CameraZoom).f == doctest::Approx(500));
    CHECK(arena.camera(cam).zoom == doctest::Approx(500));

    setProperty(arena, cam, prop::CameraTarget, Value::ofVec3(Vec3{10, 20, 30}));
    Value target = getProperty(arena, cam, prop::CameraTarget);
    CHECK(target.v3.x == doctest::Approx(10));
    CHECK(target.v3.y == doctest::Approx(20));
    CHECK(target.v3.z == doctest::Approx(30));

    setProperty(arena, cam, prop::CameraNear, Value::ofFloat(2));
    setProperty(arena, cam, prop::CameraFar, Value::ofFloat(5000));
    CHECK(getProperty(arena, cam, prop::CameraNear).f == doctest::Approx(2));
    CHECK(getProperty(arena, cam, prop::CameraFar).f == doctest::Approx(5000));
    CHECK((arena.dirty(cam) & kDirtyTransform) != 0);
}

TEST_CASE("Group node: Opacity, Translate/TranslateX interplay, Visible, dirty bits") {
    NodeArena arena;
    NodeId n = arena.create(NodeKind::Group);
    arena.clearAllDirty();

    setProperty(arena, n, prop::Opacity, Value::ofFloat(0.5f));
    CHECK(getProperty(arena, n, prop::Opacity).f == doctest::Approx(0.5f));
    CHECK((arena.dirty(n) & kDirtyPaint) != 0);

    setProperty(arena, n, prop::Translate, Value::ofVec2(Vec2{3, 4}));
    CHECK(getProperty(arena, n, prop::TranslateX).f == doctest::Approx(3));
    CHECK(getProperty(arena, n, prop::TranslateY).f == doctest::Approx(4));
    CHECK((arena.dirty(n) & kDirtyTransform) != 0);
    arena.clearAllDirty();

    setProperty(arena, n, prop::TranslateX, Value::ofFloat(5));
    Value t = getProperty(arena, n, prop::Translate);
    CHECK(t.v2.x == doctest::Approx(5));
    CHECK(t.v2.y == doctest::Approx(4));
    CHECK((arena.dirty(n) & kDirtyTransform) != 0);

    setProperty(arena, n, prop::Visible, Value::ofEnum(0));
    CHECK(getProperty(arena, n, prop::Visible).e == 0u);
    CHECK(arena.common(n).visible == 0);
    setProperty(arena, n, prop::Visible, Value::ofEnum(1));
    CHECK(getProperty(arena, n, prop::Visible).e == 1u);
}

TEST_CASE("Element node: TranslateX/Y take px, % and the mixed (px, %) form (T-211)") {
    NodeArena arena;
    NodeId e = arena.create(NodeKind::Element);
    arena.clearAllDirty();

    setProperty(arena, e, prop::TranslateX, Value::ofLength(-50, LengthUnit::Percent));
    CHECK(arena.element(e).cssTranslate.x == doctest::Approx(0));
    CHECK(arena.element(e).cssTranslatePct.x == doctest::Approx(-50));
    CHECK((arena.dirty(e) & kDirtyTransform) != 0);
    CHECK(getProperty(arena, e, prop::TranslateX).f == doctest::Approx(0));  // px part only

    setProperty(arena, e, prop::TranslateY, Value::ofVec2(Vec2{4, 25}));
    CHECK(arena.element(e).cssTranslate.y == doctest::Approx(4));
    CHECK(arena.element(e).cssTranslatePct.y == doctest::Approx(25));

    // A Float (px) write replaces the whole component, % included.
    setProperty(arena, e, prop::TranslateX, Value::ofFloat(12));
    CHECK(arena.element(e).cssTranslate.x == doctest::Approx(12));
    CHECK(arena.element(e).cssTranslatePct.x == doctest::Approx(0));
    setProperty(arena, e, prop::Translate, Value::ofVec2(Vec2{1, 2}));
    CHECK(arena.element(e).cssTranslatePct == Vec2{0, 0});
}

TEST_CASE("Element node: an effect parameter write without that effect is a latched no-op (T-211)") {
    NodeArena arena;
    NodeId e = arena.create(NodeKind::Element);
    arena.clearAllDirty();
    REQUIRE(arena.common(e).effectCount == 0);
    CHECK_FALSE(arena.effectMissingWritten());
    setProperty(arena, e, effectParam(1, 1), Value::ofFloat(4));  // No assert, Debug or Release.
    CHECK(arena.effectMissingWritten());
    CHECK(arena.common(e).effectCount == 0);
    CHECK(getProperty(arena, e, effectParam(1, 1)).f == doctest::Approx(0));
    arena.clearEffectMissingWritten();
    CHECK_FALSE(arena.effectMissingWritten());
}

TEST_CASE("Element node: Translate writes cssTranslate, Width, defaults") {
    NodeArena arena;
    NodeId e = arena.create(NodeKind::Element);
    arena.clearAllDirty();

    setProperty(arena, e, prop::Translate, Value::ofVec2(Vec2{7, 8}));
    CHECK(arena.element(e).cssTranslate == Vec2{7, 8});
    CHECK(arena.common(e).translate == Vec2{0, 0});
    Value t = getProperty(arena, e, prop::Translate);
    CHECK(t.v2 == Vec2{7, 8});

    setProperty(arena, e, prop::Width, Value::ofLength(10, LengthUnit::Px));
    Value w = getProperty(arena, e, prop::Width);
    CHECK(w.type == ValueType::Length);
    CHECK(w.len.value == doctest::Approx(10));
    CHECK(w.len.unit == LengthUnit::Px);
    CHECK((arena.dirty(e) & kDirtyLayout) != 0);

    NodeId fresh = arena.create(NodeKind::Element);
    Value width = getProperty(arena, fresh, prop::Width);
    CHECK(width.len.unit == LengthUnit::Auto);
    Value flexShrink = getProperty(arena, fresh, prop::FlexShrink);
    CHECK(flexShrink.f == doctest::Approx(1));
    Value fontSize = getProperty(arena, fresh, prop::FontSize);
    CHECK(fontSize.len.value == doctest::Approx(16));
    CHECK(fontSize.len.unit == LengthUnit::Px);
    Value color = getProperty(arena, fresh, prop::Color);
    CHECK(color.c == Color{0, 0, 0, 1});
    Value bg = getProperty(arena, fresh, prop::BackgroundColor);
    CHECK(bg.c.a == doctest::Approx(0));
}

TEST_CASE("Path node: PathData/DashArray/FillGradientStops/FillOpacity") {
    NodeArena arena;
    NodeId p = arena.create(NodeKind::Path);
    PathNodeData& path = arena.path(p);
    path.points = {0, 0, 1, 1};
    // Shared stop pool: 2 fill stops followed by 2 stroke stops.
    path.stops = {
        {0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}, {0, Color{0, 0, 1, 1}}, {1, Color{1, 1, 0, 1}}};
    path.fill.stopOffset = 0;
    path.fill.stopCount = 2;
    path.stroke.stopOffset = 2;
    path.stroke.stopCount = 2;
    arena.clearAllDirty();

    float newPts[4] = {2, 2, 3, 3};
    setProperty(arena, p, prop::PathData, Value::ofPath(newPts, 4));
    CHECK(path.points[0] == doctest::Approx(2));
    CHECK(path.points[3] == doctest::Approx(3));

    setProperty(arena, p, prop::DashArray, Value::ofFloatArray(nullptr, 0));
    CHECK(path.dashes.empty());
    float dashes[3] = {1, 2, 3};
    setProperty(arena, p, prop::DashArray, Value::ofFloatArray(dashes, 3));
    CHECK(path.dashes.size() == 3u);
    CHECK(path.strokeStyle.dashCount == 3u);
    Value dashOut = getProperty(arena, p, prop::DashArray);
    CHECK(dashOut.floatCount == 3u);

    GradientStop newStrokeStops[2] = {{0.25f, Color{1, 1, 1, 1}}, {0.75f, Color{0, 0, 0, 1}}};
    setProperty(arena, p, prop::StrokeGradientStops, Value::ofStops(newStrokeStops, 2));
    CHECK(path.stops[2].offset == doctest::Approx(0.25f));
    CHECK(path.stops[3].offset == doctest::Approx(0.75f));
    // Writing stroke stops must not touch the fill segment.
    CHECK(path.stops[0].offset == doctest::Approx(0));
    CHECK(path.stops[1].offset == doctest::Approx(1));

    Value fillStopsOut = getProperty(arena, p, prop::FillGradientStops);
    CHECK(fillStopsOut.stopCount == 2u);
    CHECK(fillStopsOut.stops[0].offset == doctest::Approx(0));

    setProperty(arena, p, prop::FillOpacity, Value::ofFloat(0.3f));
    CHECK(getProperty(arena, p, prop::FillOpacity).f == doctest::Approx(0.3f));
    CHECK(path.fillOpacity == doctest::Approx(0.3f));
}

// docs/tasks/T-50.md: effect-parameter PropIds (docs/design/18-effects.md §18.2-18.3).

TEST_CASE("effectParam: encode/decode and isEffectProp") {
    CHECK(effectParam(0, 0) == static_cast<PropId>(0x8000));
    CHECK(effectParam(3, 5) == static_cast<PropId>(0x8000 | (3 << 8) | 5));
    CHECK(effectParam(15, prop::kEffectParamOffset) == static_cast<PropId>(0x8000 | (15 << 8) | 18));
    CHECK(isEffectProp(effectParam(0, 0)));
    CHECK(isEffectProp(static_cast<PropId>(0x8000)));
    CHECK_FALSE(isEffectProp(static_cast<PropId>(0x7FFF)));
    CHECK_FALSE(isEffectProp(prop::Opacity));
}

TEST_CASE("propertyDefined: effect params -- valid set, p = 9/19 and i = 16 are undefined") {
    CHECK(propertyDefined(effectParam(0, prop::kEffectParamEnabled)));
    for (uint32_t p = 1; p <= 8; ++p) {
        CAPTURE(p);
        CHECK(propertyDefined(effectParam(0, p)));
    }
    CHECK(propertyDefined(effectParam(0, prop::kEffectParamColor0)));
    CHECK(propertyDefined(effectParam(0, prop::kEffectParamColor1)));
    CHECK(propertyDefined(effectParam(0, prop::kEffectParamOffset)));
    CHECK(propertyDefined(effectParam(15, 0)));  // Highest valid effectIndex.

    CHECK_FALSE(propertyDefined(effectParam(0, 9)));
    CHECK_FALSE(propertyDefined(effectParam(0, 19)));
    // effectIndex = 16: effectParam() itself masks effectIndex to 0-15, so build the PropId
    // directly to exercise propertyDefined()'s "i <= 15" check.
    PropId badIndex = static_cast<PropId>(0x8000 | (16 << 8) | 0);
    CHECK_FALSE(propertyDefined(badIndex));
}

TEST_CASE("propertyType/propertyDirtyBits: effect params") {
    CHECK(propertyType(effectParam(0, prop::kEffectParamEnabled)) == ValueType::Float);
    CHECK(propertyType(effectParam(0, 3)) == ValueType::Float);
    CHECK(propertyType(effectParam(0, prop::kEffectParamColor0)) == ValueType::Color);
    CHECK(propertyType(effectParam(0, prop::kEffectParamColor1)) == ValueType::Color);
    CHECK(propertyType(effectParam(0, prop::kEffectParamOffset)) == ValueType::Vec2);

    CHECK(propertyDirtyBits(effectParam(0, 0)) == kDirtyPaint);
    CHECK(propertyDirtyBits(effectParam(2, prop::kEffectParamColor0)) == kDirtyPaint);
}

TEST_CASE("propertyApplies: effect params apply to every drawable kind, not Camera/Free") {
    CHECK(propertyApplies(NodeKind::Group, effectParam(0, 0)));
    CHECK(propertyApplies(NodeKind::Path, effectParam(0, 0)));
    CHECK(propertyApplies(NodeKind::Image, effectParam(0, 0)));
    CHECK(propertyApplies(NodeKind::Text, effectParam(0, 0)));
    CHECK(propertyApplies(NodeKind::Element, effectParam(0, 0)));
    CHECK(propertyApplies(NodeKind::Mask, effectParam(0, 0)));
    CHECK_FALSE(propertyApplies(NodeKind::Camera, effectParam(0, 0)));
    CHECK_FALSE(propertyApplies(NodeKind::Free, effectParam(0, 0)));
}

TEST_CASE("Effect params: get/set round trip on a Group with 2 effects, out-of-range default, names") {
    NodeArena arena;
    NodeId g = arena.create(NodeKind::Group);

    EffectInstance fx0{};
    fx0.kind = EffectKind::DropShadow;
    EffectInstance fx1{};
    fx1.kind = EffectKind::Glow;
    uint32_t base = static_cast<uint32_t>(arena.effects().size());
    arena.effects().push_back(fx0);
    arena.effects().push_back(fx1);
    arena.setEffectRange(g, base, 2);  // T-270c: the only way in besides assignEffects().
    arena.clearAllDirty();

    // paramIndex 0: enabled.
    setProperty(arena, g, effectParam(0, prop::kEffectParamEnabled), Value::ofFloat(0));
    CHECK(getProperty(arena, g, effectParam(0, prop::kEffectParamEnabled)).f == doctest::Approx(0));
    CHECK(arena.effects()[base].enabled == 0);
    CHECK((arena.dirty(g) & kDirtyPaint) != 0);

    // paramIndex 4 -> params[3].
    setProperty(arena, g, effectParam(0, 4), Value::ofFloat(2.5f));
    CHECK(getProperty(arena, g, effectParam(0, 4)).f == doctest::Approx(2.5f));
    CHECK(arena.effects()[base].params[3] == doctest::Approx(2.5f));

    // colors[1] on effect index 1.
    setProperty(arena, g, effectParam(1, prop::kEffectParamColor1),
                Value::ofColor(Color{0.1f, 0.2f, 0.3f, 0.4f}));
    Value c1 = getProperty(arena, g, effectParam(1, prop::kEffectParamColor1));
    CHECK(c1.c.r == doctest::Approx(0.1f));
    CHECK(c1.c.g == doctest::Approx(0.2f));
    CHECK(arena.effects()[base + 1].colors[1].b == doctest::Approx(0.3f));

    // Composite Vec2 offset (params[0..1]) on effect index 0.
    setProperty(arena, g, effectParam(0, prop::kEffectParamOffset), Value::ofVec2(Vec2{3, 4}));
    Value offset = getProperty(arena, g, effectParam(0, prop::kEffectParamOffset));
    CHECK(offset.v2.x == doctest::Approx(3));
    CHECK(offset.v2.y == doctest::Approx(4));
    CHECK(arena.effects()[base].params[0] == doctest::Approx(3));
    CHECK(arena.effects()[base].params[1] == doctest::Approx(4));

    // effectIndex = 5 doesn't exist on this node (effectCount == 2) -> type-appropriate default.
    CHECK(getProperty(arena, g, effectParam(5, 3)).f == doctest::Approx(0));
    Value defColor = getProperty(arena, g, effectParam(5, prop::kEffectParamColor0));
    CHECK(defColor.c.a == doctest::Approx(0));
    Value defOffset = getProperty(arena, g, effectParam(5, prop::kEffectParamOffset));
    CHECK(defOffset.v2.x == doctest::Approx(0));
    CHECK(defOffset.v2.y == doctest::Approx(0));
    // Note: setProperty() on an out-of-range effectIndex is a Debug-build XT_ASSERT (not silently
    // ignored the way Release's XT_ASSERT no-op would make it look); not exercised here since
    // doctest runs in a Debug build and XT_ASSERT aborts.

    // propertyName(prop) alone has no arena/node context, so it always uses the generic form.
    CHECK(std::strcmp(propertyName(effectParam(0, 3)), "effect[0].param3") == 0);
    // propertyName(arena, node, prop) resolves DropShadow's paramIndex 3 to "blur" (18.3).
    CHECK(std::strcmp(propertyName(arena, g, effectParam(0, 3)), "effect[0].blur") == 0);
    // Glow (effect index 1) paramIndex 1 -> "radius".
    CHECK(std::strcmp(propertyName(arena, g, effectParam(1, 1)), "effect[1].radius") == 0);
    // effectIndex = 5 doesn't exist on this node -> falls back to the generic form too.
    CHECK(std::strcmp(propertyName(arena, g, effectParam(5, 3)), "effect[5].param3") == 0);
}

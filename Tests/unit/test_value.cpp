#include <type_traits>

#include "doctest.h"
#include "xt/types.h"
#include "xt/value.h"

using namespace xt;

static_assert(std::is_trivially_copyable_v<Value>, "Value must be trivially copyable");

TEST_CASE("Value: of* constructors set type and payload") {
    Value f = Value::ofFloat(3.5f);
    CHECK(f.type == ValueType::Float);
    CHECK(f.f == doctest::Approx(3.5f));

    Value v2 = Value::ofVec2(Vec2{1, 2});
    CHECK(v2.type == ValueType::Vec2);
    CHECK(v2.v2 == Vec2{1, 2});

    Value v3 = Value::ofVec3(Vec3{1, 2, 3});
    CHECK(v3.type == ValueType::Vec3);
    CHECK(v3.v3.x == doctest::Approx(1));
    CHECK(v3.v3.y == doctest::Approx(2));
    CHECK(v3.v3.z == doctest::Approx(3));

    Value c = Value::ofColor(Color{0.1f, 0.2f, 0.3f, 0.4f});
    CHECK(c.type == ValueType::Color);
    CHECK(c.c == Color{0.1f, 0.2f, 0.3f, 0.4f});

    Value len = Value::ofLength(10, LengthUnit::Percent);
    CHECK(len.type == ValueType::Length);
    CHECK(len.len.value == doctest::Approx(10));
    CHECK(len.len.unit == LengthUnit::Percent);

    Value e = Value::ofEnum(7);
    CHECK(e.type == ValueType::Enum);
    CHECK(e.e == 7u);

    Value sid = Value::ofStringId(42);
    CHECK(sid.type == ValueType::StringRef);
    CHECK(sid.sid == 42u);

    float pts[4] = {0, 0, 1, 1};
    Value path = Value::ofPath(pts, 4);
    CHECK(path.type == ValueType::Path);
    CHECK(path.floats == pts);
    CHECK(path.floatCount == 4u);

    float arr[3] = {1, 2, 3};
    Value fa = Value::ofFloatArray(arr, 3);
    CHECK(fa.type == ValueType::FloatArray);
    CHECK(fa.floats == arr);
    CHECK(fa.floatCount == 3u);

    GradientStop stops[2] = {{0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}};
    Value st = Value::ofStops(stops, 2);
    CHECK(st.type == ValueType::Stops);
    CHECK(st.stops == stops);
    CHECK(st.stopCount == 2u);
}

TEST_CASE("Value: neutral marker") {
    Value n = Value::neutral();
    CHECK(n.type == ValueType::Enum);
    CHECK(n.e == 0xFFFFFFFFu);
    CHECK(n.isNeutral());

    CHECK_FALSE(Value::ofEnum(0).isNeutral());
    CHECK_FALSE(Value::ofFloat(0).isNeutral());
}

TEST_CASE("valueEquals: same type equal/unequal") {
    CHECK(valueEquals(Value::ofFloat(1), Value::ofFloat(1)));
    CHECK_FALSE(valueEquals(Value::ofFloat(1), Value::ofFloat(2)));

    CHECK(valueEquals(Value::ofVec2(Vec2{1, 2}), Value::ofVec2(Vec2{1, 2})));
    CHECK_FALSE(valueEquals(Value::ofVec2(Vec2{1, 2}), Value::ofVec2(Vec2{1, 3})));

    CHECK(valueEquals(Value::ofVec3(Vec3{1, 2, 3}), Value::ofVec3(Vec3{1, 2, 3})));
    CHECK_FALSE(valueEquals(Value::ofVec3(Vec3{1, 2, 3}), Value::ofVec3(Vec3{1, 2, 4})));

    CHECK(valueEquals(Value::ofColor(Color{1, 0, 0, 1}), Value::ofColor(Color{1, 0, 0, 1})));
    CHECK_FALSE(valueEquals(Value::ofColor(Color{1, 0, 0, 1}), Value::ofColor(Color{1, 0, 0, 0.5f})));

    CHECK(valueEquals(Value::ofLength(1, LengthUnit::Px), Value::ofLength(1, LengthUnit::Px)));
    CHECK_FALSE(valueEquals(Value::ofLength(1, LengthUnit::Px), Value::ofLength(1, LengthUnit::Percent)));

    CHECK(valueEquals(Value::ofEnum(3), Value::ofEnum(3)));
    CHECK_FALSE(valueEquals(Value::ofEnum(3), Value::ofEnum(4)));
}

TEST_CASE("valueEquals: different types are unequal") {
    CHECK_FALSE(valueEquals(Value::ofFloat(1), Value::ofEnum(1)));
    CHECK_FALSE(valueEquals(Value::ofVec2(Vec2{1, 1}), Value::ofFloat(1)));
}

TEST_CASE("valueEquals: Path/FloatArray/Stops element-wise") {
    float ptsA[4] = {0, 0, 1, 1};
    float ptsB[4] = {0, 0, 1, 1};
    float ptsC[4] = {0, 0, 1, 2};
    CHECK(valueEquals(Value::ofPath(ptsA, 4), Value::ofPath(ptsB, 4)));
    CHECK_FALSE(valueEquals(Value::ofPath(ptsA, 4), Value::ofPath(ptsC, 4)));

    float ptsShort[2] = {0, 0};
    CHECK_FALSE(valueEquals(Value::ofPath(ptsA, 4), Value::ofPath(ptsShort, 2)));

    GradientStop stopsA[2] = {{0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}};
    GradientStop stopsB[2] = {{0, Color{1, 0, 0, 1}}, {1, Color{0, 1, 0, 1}}};
    CHECK(valueEquals(Value::ofStops(stopsA, 2), Value::ofStops(stopsB, 2)));

    GradientStop stopsC[1] = {{0, Color{1, 0, 0, 1}}};
    CHECK_FALSE(valueEquals(Value::ofStops(stopsA, 2), Value::ofStops(stopsC, 1)));
}

TEST_CASE("lerpValue: Float midpoint") {
    Value out = lerpValue(Value::ofFloat(0), Value::ofFloat(10), 0.5f, nullptr, nullptr);
    CHECK(out.f == doctest::Approx(5));
}

TEST_CASE("lerpValue: Vec2") {
    Value out = lerpValue(Value::ofVec2(Vec2{0, 0}), Value::ofVec2(Vec2{10, 20}), 0.5f, nullptr, nullptr);
    CHECK(out.v2.x == doctest::Approx(5));
    CHECK(out.v2.y == doctest::Approx(10));
}

TEST_CASE("lerpValue: Vec3 midpoint") {
    Value out =
        lerpValue(Value::ofVec3(Vec3{0, 0, 0}), Value::ofVec3(Vec3{10, 20, 30}), 0.5f, nullptr, nullptr);
    CHECK(out.v3.x == doctest::Approx(5));
    CHECK(out.v3.y == doctest::Approx(10));
    CHECK(out.v3.z == doctest::Approx(15));
}

TEST_CASE("lerpValue: Color") {
    Value out = lerpValue(Value::ofColor(Color{0, 0, 0, 0}), Value::ofColor(Color{1, 1, 1, 1}), 0.25f,
                          nullptr, nullptr);
    CHECK(out.c.r == doctest::Approx(0.25f));
    CHECK(out.c.g == doctest::Approx(0.25f));
    CHECK(out.c.b == doctest::Approx(0.25f));
    CHECK(out.c.a == doctest::Approx(0.25f));
}

TEST_CASE("lerpValue: Length same unit linear, different unit discrete, Auto discrete") {
    Value a = Value::ofLength(0, LengthUnit::Px);
    Value b = Value::ofLength(10, LengthUnit::Px);
    Value out = lerpValue(a, b, 0.5f, nullptr, nullptr);
    CHECK(out.len.value == doctest::Approx(5));
    CHECK(out.len.unit == LengthUnit::Px);

    Value pct = Value::ofLength(10, LengthUnit::Percent);
    Value below = lerpValue(a, pct, 0.49f, nullptr, nullptr);
    CHECK(below.len.value == doctest::Approx(0));
    CHECK(below.len.unit == LengthUnit::Px);
    Value atHalf = lerpValue(a, pct, 0.5f, nullptr, nullptr);
    CHECK(atHalf.len.value == doctest::Approx(10));
    CHECK(atHalf.len.unit == LengthUnit::Percent);

    Value autoA = Value::ofLength(0, LengthUnit::Auto);
    Value autoB = Value::ofLength(0, LengthUnit::Auto);
    Value autoOut = lerpValue(autoA, autoB, 0.9f, nullptr, nullptr);
    CHECK(autoOut.len.unit == LengthUnit::Auto);  // same unit but Auto -> still discrete
}

TEST_CASE("lerpValue: Path same length interpolates, different length discrete") {
    float ptsA[4] = {0, 0, 10, 10};
    float ptsB[4] = {10, 10, 20, 20};
    float scratch[4];
    Value out = lerpValue(Value::ofPath(ptsA, 4), Value::ofPath(ptsB, 4), 0.5f, scratch, nullptr);
    CHECK(out.floats == scratch);
    CHECK(out.floatCount == 4u);
    CHECK(scratch[0] == doctest::Approx(5));
    CHECK(scratch[1] == doctest::Approx(5));
    CHECK(scratch[2] == doctest::Approx(15));
    CHECK(scratch[3] == doctest::Approx(15));

    float ptsShort[2] = {1, 1};
    Value discreteLow =
        lerpValue(Value::ofPath(ptsA, 4), Value::ofPath(ptsShort, 2), 0.49f, scratch, nullptr);
    CHECK(discreteLow.floats == ptsA);
    Value discreteHigh =
        lerpValue(Value::ofPath(ptsA, 4), Value::ofPath(ptsShort, 2), 0.5f, scratch, nullptr);
    CHECK(discreteHigh.floats == ptsShort);
}

TEST_CASE("lerpValue: Stops same count interpolates, different count discrete") {
    GradientStop stopsA[2] = {{0, Color{0, 0, 0, 0}}, {1, Color{0, 0, 0, 0}}};
    GradientStop stopsB[2] = {{0, Color{1, 1, 1, 1}}, {1, Color{1, 1, 1, 1}}};
    GradientStop scratch[2];
    Value out = lerpValue(Value::ofStops(stopsA, 2), Value::ofStops(stopsB, 2), 0.5f, nullptr, scratch);
    CHECK(out.stops == scratch);
    CHECK(out.stopCount == 2u);
    CHECK(scratch[0].offset == doctest::Approx(0));
    CHECK(scratch[1].offset == doctest::Approx(1));
    CHECK(scratch[0].color.r == doctest::Approx(0.5f));

    GradientStop stopsC[1] = {{0, Color{0, 0, 0, 0}}};
    Value discrete = lerpValue(Value::ofStops(stopsA, 2), Value::ofStops(stopsC, 1), 0.5f, nullptr, scratch);
    CHECK(discrete.stops == stopsC);
    CHECK(discrete.stopCount == 1u);
}

TEST_CASE("lerpValue: FloatArray") {
    float a[3] = {0, 0, 0};
    float b[3] = {3, 6, 9};
    float scratch[3];
    Value out =
        lerpValue(Value::ofFloatArray(a, 3), Value::ofFloatArray(b, 3), 1.0f / 3.0f, scratch, nullptr);
    CHECK(out.floats == scratch);
    CHECK(scratch[0] == doctest::Approx(1));
    CHECK(scratch[1] == doctest::Approx(2));
    CHECK(scratch[2] == doctest::Approx(3));
}

TEST_CASE("lerpValue: Enum always discrete") {
    Value a = Value::ofEnum(1);
    Value b = Value::ofEnum(2);
    CHECK(lerpValue(a, b, 0.0f, nullptr, nullptr).e == 1u);
    CHECK(lerpValue(a, b, 0.49f, nullptr, nullptr).e == 1u);
    CHECK(lerpValue(a, b, 0.5f, nullptr, nullptr).e == 2u);
    CHECK(lerpValue(a, b, 1.0f, nullptr, nullptr).e == 2u);
}
